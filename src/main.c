/*
 * PicoUSBKeyBridge: CDC (native USB) -> HID keyboard (PIO USB)
 *
 * Core ownership:
 * - core1 runs the PIO USB device on the USB A port (host-side HID keyboard).
 *   It services the PIO USB task and consumes key packets from the FIFO.
 * - core0 (main) handles CDC input from the native USB port and logging output.
 *   It parses CDC bytes into HID key packets and feeds core1 via the FIFO.
 *
 * Inter-core communication:
 * - core0 enqueues 16-bit key packets (keycode + modifier) into a small ring
 *   buffer, then pushes them over the multicore FIFO when space is available.
 * - core1 pops packets from the FIFO and emits HID reports over the PIO USB
 *   device stack.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "class/hid/hid_device.h"
#include "device/usbd.h"
#include "log.h"
#include "pio_usb.h"
#include "tusb.h"

// --------------------------------------------------------------------
// Logging (CDC TX)
// --------------------------------------------------------------------

static void cdc_log(const char *level, const char *message) {
  log_write(level, strlen(level));
  log_write(message, strlen(message));
  log_write("\r\n", 2);
}

static void cdc_log_hex2(const char *prefix, uint8_t a, uint8_t b) {
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%s%02X %02X\r\n", prefix, a, b);
  if (len > 0) {
    size_t write_len = (size_t)len;
    if (write_len >= sizeof(buf)) {
      write_len = sizeof(buf) - 1;
    }
    log_write(buf, write_len);
  }
}

#define LOG_INFO(msg) cdc_log("INFO: ", (msg))
#define LOG_WARN(msg) cdc_log("WARN: ", (msg))
#define LOG_ERROR(msg) cdc_log("ERROR: ", (msg))
#if PUSBKB_DEBUG
#define LOG_DEBUG(msg) cdc_log("DEBUG: ", (msg))
#define LOG_DEBUG_PKT(a, b) cdc_log_hex2("DEBUG: rx ", (a), (b))
#else
#define LOG_DEBUG(msg) do {} while (0)
#define LOG_DEBUG_PKT(a, b) do {} while (0)
#endif

// --------------------------------------------------------------------
// Watchdog configuration
// --------------------------------------------------------------------

// Watchdog timeout in milliseconds. Both cores must be responsive within this
// window or the device will reboot. 8 seconds is generous; the main loops
// should iterate in milliseconds.
#define WATCHDOG_TIMEOUT_MS 8000

// Core1 updates this timestamp each iteration. Core0 checks it before petting
// the watchdog. If core1 stops updating, core0 stops petting, and the watchdog
// reboots the device.
#define CORE1_HEALTH_TIMEOUT_US 5000000  // 5 seconds
static volatile uint32_t core1_last_seen_us = 0;
static volatile bool core1_initialized = false;

// --------------------------------------------------------------------
// PIO USB HID descriptors (keyboard only)
// --------------------------------------------------------------------

static usb_device_t *pio_usb_device = NULL;
static endpoint_t *pio_hid_ep = NULL;

static tusb_desc_device_t const pio_desc_device = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0110,
  .bDeviceClass       = 0x00,
  .bDeviceSubClass    = 0x00,
  .bDeviceProtocol    = 0x00,
  .bMaxPacketSize0    = 64,
  .idVendor           = 0xCafe,
  .idProduct          = 0x0001,
  .bcdDevice          = 0x0100,
  .iManufacturer      = 0x01,
  .iProduct           = 0x02,
  .iSerialNumber      = 0x03,
  .bNumConfigurations = 0x01
};

enum {
  ITF_NUM_KEYBOARD = 0,
  ITF_NUM_TOTAL,
};

enum {
  EPNUM_KEYBOARD = 0x81,
};

static uint8_t const desc_hid_keyboard_report[] = {
  TUD_HID_REPORT_DESC_KEYBOARD()
};

static const uint8_t *report_desc[] = {
  desc_hid_keyboard_report
};

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static uint8_t const pio_desc_configuration[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
  TUD_HID_DESCRIPTOR(ITF_NUM_KEYBOARD, 0, HID_ITF_PROTOCOL_KEYBOARD,
                     sizeof(desc_hid_keyboard_report), EPNUM_KEYBOARD,
                     CFG_TUD_HID_EP_BUFSIZE, 10),
};

static const char *pio_usb_string_descriptors_base[] = {
  [0] = (const char[]){0x09, 0x04},
  [1] = "PicoUSBKeyBridge",
  [2] = "PicoUSBKeyBridge PIO HID",
  [3] = "000000000002",
};

static string_descriptor_t pio_usb_string_desc[4];

static void init_pio_usb_string_desc(void) {
  for (int idx = 0; idx < 4; idx++) {
    uint8_t len = 0;
    uint16_t *wchar_str = (uint16_t *)&pio_usb_string_desc[idx];
    if (idx == 0) {
      wchar_str[1] = pio_usb_string_descriptors_base[0][0] |
                     ((uint16_t)pio_usb_string_descriptors_base[0][1] << 8);
      len = 1;
    } else if (idx <= 3) {
      len = strnlen(pio_usb_string_descriptors_base[idx], 31);
      for (int i = 0; i < len; i++) {
        wchar_str[i + 1] = pio_usb_string_descriptors_base[idx][i];
      }
    }
    wchar_str[0] = (TUSB_DESC_STRING << 8) | (2 * len + 2);
  }
}

static usb_descriptor_buffers_t pio_desc = {
  .device = (uint8_t *)&pio_desc_device,
  .config = pio_desc_configuration,
  .hid_report = report_desc,
  .string = pio_usb_string_desc
};

// --------------------------------------------------------------------
// CDC protocol -> HID mapping
// --------------------------------------------------------------------

typedef struct {
  uint8_t keycode;
  uint8_t modifier;
} hid_key_t;

// Small ring buffer to absorb CDC bursts without blocking USB tasks.
// This lives on core0 and feeds the multicore FIFO opportunistically.
#define PUSBKB_QUEUE_LEN 64
static uint16_t key_queue[PUSBKB_QUEUE_LEN];
static uint16_t key_queue_head = 0;
static uint16_t key_queue_tail = 0;

static bool key_queue_is_empty(void) {
  return key_queue_head == key_queue_tail;
}

static size_t key_queue_free_space(void) {
  if (key_queue_head >= key_queue_tail) {
    return PUSBKB_QUEUE_LEN - (key_queue_head - key_queue_tail) - 1;
  }
  return (key_queue_tail - key_queue_head) - 1;
}

static bool key_queue_push(uint16_t packed) {
  if (key_queue_free_space() == 0) {
    return false;
  }
  key_queue[key_queue_head] = packed;
  key_queue_head = (uint16_t)((key_queue_head + 1) % PUSBKB_QUEUE_LEN);
  return true;
}

static bool key_queue_pop(uint16_t *out) {
  if (key_queue_is_empty()) {
    return false;
  }
  *out = key_queue[key_queue_tail];
  key_queue_tail = (uint16_t)((key_queue_tail + 1) % PUSBKB_QUEUE_LEN);
  return true;
}

static void pio_send_key(const hid_key_t *key) {
  if (pio_usb_device == NULL || pio_hid_ep == NULL) {
    return;
  }

  // HID report on the PIO USB (USB A port) device stack.
  hid_keyboard_report_t report = {0};
  report.modifier = key->modifier;
  report.keycode[0] = key->keycode;
  pio_usb_set_out_data(pio_hid_ep, (uint8_t *)&report, sizeof(report));
  sleep_ms(2);

  hid_keyboard_report_t release = {0};
  pio_usb_set_out_data(pio_hid_ep, (uint8_t *)&release, sizeof(release));
  sleep_ms(2);
}

// --------------------------------------------------------------------
// Core1: USB A port (PIO USB device) + HID report output
// --------------------------------------------------------------------

void core1_main() {
  sleep_ms(10);

  pio_usb_configuration_t pio_cfg = {
    .pin_dp = 12,
    .pio_tx_num = 0,
    .sm_tx = 0,
    .tx_ch = 0,
    .pio_rx_num = 1,
    .sm_rx = 0,
    .sm_eop = 1,
    .alarm_pool = NULL,
    .debug_pin_rx = PIO_USB_DEBUG_PIN_NONE,
    .debug_pin_eop = PIO_USB_DEBUG_PIN_NONE,
    .skip_alarm_pool = false,
    .pinout = PIO_USB_PINOUT_DPDM
  };

  init_pio_usb_string_desc();
  // Initialize the PIO USB device (USB A port).
  pio_usb_device = pio_usb_device_init(&pio_cfg, &pio_desc);
  if (pio_usb_device == NULL) {
    LOG_ERROR("PIO USB init failed - waiting for watchdog");
    // Don't update core1_last_seen_us; watchdog will trigger reboot
    while (true) {
      sleep_ms(100);
    }
  }
  // Grab the HID endpoint from the PIO USB device stack.
  pio_hid_ep = pio_usb_get_endpoint(pio_usb_device, 1);
  if (pio_hid_ep == NULL) {
    LOG_ERROR("PIO USB HID endpoint missing - waiting for watchdog");
    // Don't update core1_last_seen_us; watchdog will trigger reboot
    while (true) {
      sleep_ms(100);
    }
  }
  LOG_INFO("PIO USB HID initialized");

  // Signal that core1 is healthy and ready
  core1_initialized = true;
  core1_last_seen_us = time_us_32();

  while (true) {
    // Update health timestamp for watchdog monitoring
    core1_last_seen_us = time_us_32();

    // Keep the PIO USB device running on the USB A port.
    pio_usb_device_task();

    // Consume key packets from core0 and emit HID reports over PIO USB.
    while (multicore_fifo_rvalid()) {
      uint32_t packed = multicore_fifo_pop_blocking();
      hid_key_t key = {
        .keycode = (uint8_t)(packed & 0xFF),
        .modifier = (uint8_t)((packed >> 8) & 0xFF),
      };
      if (key.keycode != 0) {
        LOG_DEBUG_PKT(key.keycode, key.modifier);
        pio_send_key(&key);
      }
    }
  }
}

// --------------------------------------------------------------------
// Core0: CDC (native USB) input + logging output
// --------------------------------------------------------------------

typedef struct {
  bool was_connected;
  bool have_keycode;
  uint8_t pending_keycode;
  uint32_t dropped_queue;
  absolute_time_t last_rx_time;
  bool last_rx_time_valid;
} cdc_rx_state_t;

// Check if core1 is healthy (responsive within timeout)
static bool core1_is_healthy(void) {
  if (!core1_initialized) {
    // Core1 hasn't finished init yet; give it time during startup
    return true;
  }
  uint32_t now = time_us_32();
  uint32_t last_seen = core1_last_seen_us;
  // Handle timer wraparound (occurs every ~72 minutes)
  uint32_t elapsed = now - last_seen;
  return elapsed < CORE1_HEALTH_TIMEOUT_US;
}

static void core0_pet_watchdog_if_healthy(void) {
  // Pet the watchdog only if both cores are healthy.
  // If core1 stops responding, we stop petting, and watchdog reboots us.
  if (core1_is_healthy()) {
    watchdog_update();
  }
}

static void core0_service_native_usb_and_logs(void) {
  // Service native USB (CDC) tasks and flush any pending log output.
  tud_task();
  log_flush();
}

static void core0_drain_queue_to_fifo(void) {
  // Drain queued packets into the multicore FIFO when space is available.
  // Use timeout to avoid blocking indefinitely if core1 stops consuming.
  while (multicore_fifo_wready()) {
    uint16_t packed;
    if (!key_queue_pop(&packed)) {
      break;
    }
    if (!multicore_fifo_push_timeout_us((uint32_t)packed, 100000)) {
      // Core1 not consuming; watchdog will handle if it persists.
      // Drop the packet rather than reorder by re-queuing at wrong position.
      LOG_DEBUG("FIFO push timeout, packet dropped");
      break;
    }
  }
}

static void core0_update_cdc_state(cdc_rx_state_t *state) {
  bool connected = tud_cdc_connected();  // CDC on native USB port.
  if (state->was_connected && !connected) {
    // Drop partial packets on disconnect to avoid stale pairing later.
    state->have_keycode = false;
    state->pending_keycode = 0;
    state->last_rx_time_valid = false;
  }
  state->was_connected = connected;
  if (state->have_keycode && state->last_rx_time_valid) {
    int64_t age_us = absolute_time_diff_us(state->last_rx_time,
                                           get_absolute_time());
    if (age_us > 200000) {
      // If a second byte never arrives, discard the pending keycode.
      state->have_keycode = false;
      state->pending_keycode = 0;
      state->last_rx_time_valid = false;
    }
  }
}

static void core0_handle_cdc_input(cdc_rx_state_t *state) {
  // Only read from CDC when the queue has room for more packets.
  size_t free_packets = key_queue_free_space();
  size_t max_bytes = free_packets * 2;
  if (state->have_keycode) {
    // Allow one extra byte to complete a pending keycode.
    if (max_bytes == 0) {
      return;
    }
    max_bytes += 1;
  }

  if (!tud_cdc_available()) {  // CDC RX on native USB port.
    return;
  }

  uint8_t buf[64];
  size_t read_len = sizeof(buf);
  if (max_bytes < read_len) {
    read_len = max_bytes;
  }
  if (read_len == 0) {
    return;
  }

  uint32_t count = tud_cdc_read(buf, (uint32_t)read_len);  // CDC RX bytes.
  if (count > 0) {
    LOG_DEBUG("CDC RX data");
    state->last_rx_time = get_absolute_time();
    state->last_rx_time_valid = true;
  }

  for (uint32_t i = 0; i < count; i++) {
    if (!state->have_keycode) {
      state->pending_keycode = buf[i];
      state->have_keycode = true;
      continue;
    }

    uint8_t modifier = buf[i];
    uint16_t packed = (uint16_t)state->pending_keycode |
                      ((uint16_t)modifier << 8);
    if (state->pending_keycode != 0) {
      if (!key_queue_push(packed)) {
        state->dropped_queue++;
        if ((state->dropped_queue & 0x3F) == 1) {
          // Throttle noisy logs while still indicating drops.
          LOG_DEBUG("CDC RX drop: queue full");
        }
      }
    }
    state->have_keycode = false;
  }
}

int main(void) {
  set_sys_clock_khz(120000, true);

  // Initialize CDC logging before launching core1 to avoid race conditions.
  log_init();

  // Check if we rebooted due to watchdog (log after CDC connects)
  bool watchdog_reboot = watchdog_enable_caused_reboot();

  // Launch core1 to own the USB A (PIO) device stack.
  multicore_reset_core1();
  multicore_launch_core1(core1_main);

  // Wait briefly for core1 to bring up the PIO USB device stack.
  absolute_time_t core1_wait_start = get_absolute_time();
  while (!core1_initialized) {
    if (absolute_time_diff_us(core1_wait_start, get_absolute_time()) > 200000) {
      LOG_ERROR("core1 PIO USB init timeout");
      break;
    }
    sleep_ms(5);
  }
  // Initialize the native USB stack (CDC on the built-in USB port).
  if (!tud_init(0)) {
    LOG_ERROR("tud_init failed");
  }

  if (watchdog_reboot) {
    LOG_WARN("watchdog triggered reboot");
  }
  LOG_INFO("PicoUSBKeyBridge boot");
  LOG_INFO("CDC device ready");

  // Enable watchdog with timeout, pause during debug sessions
  watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
  LOG_INFO("watchdog enabled");

  cdc_rx_state_t cdc_rx_state = {0};

  while (true) {
    core0_pet_watchdog_if_healthy();
    core0_service_native_usb_and_logs();
    core0_drain_queue_to_fifo();
    core0_update_cdc_state(&cdc_rx_state);
    core0_handle_cdc_input(&cdc_rx_state);
  }

  return 0;
}
