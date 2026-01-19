/*
 * keyemu: CDC (native USB) -> HID keyboard (PIO USB)
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "class/hid/hid_device.h"
#include "device/usbd.h"
#include "keyemu_log.h"
#include "pio_usb.h"
#include "tusb.h"

// --------------------------------------------------------------------
// Logging (CDC TX)
// --------------------------------------------------------------------

static void cdc_log(const char *level, const char *message) {
  keyemu_log_write(level, strlen(level));
  keyemu_log_write(message, strlen(message));
  keyemu_log_write("\r\n", 2);
}

static void cdc_log_hex2(const char *prefix, uint8_t a, uint8_t b) {
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%s%02X %02X\r\n", prefix, a, b);
  if (len > 0) {
    size_t write_len = (size_t)len;
    if (write_len >= sizeof(buf)) {
      write_len = sizeof(buf) - 1;
    }
    keyemu_log_write(buf, write_len);
  }
}

#define LOG_INFO(msg) cdc_log("INFO: ", (msg))
#if KEYEMU_DEBUG
#define LOG_DEBUG(msg) cdc_log("DEBUG: ", (msg))
#define LOG_DEBUG_PKT(a, b) cdc_log_hex2("DEBUG: rx ", (a), (b))
#else
#define LOG_DEBUG(msg) do {} while (0)
#define LOG_DEBUG_PKT(a, b) do {} while (0)
#endif

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

static const char *string_descriptors_base[] = {
  [0] = (const char[]){0x09, 0x04},
  [1] = "keyemu",
  [2] = "keyemu PIO HID",
  [3] = "000000000002",
};

static string_descriptor_t str_desc[4];

static void init_string_desc(void) {
  for (int idx = 0; idx < 4; idx++) {
    uint8_t len = 0;
    uint16_t *wchar_str = (uint16_t *)&str_desc[idx];
    if (idx == 0) {
      wchar_str[1] = string_descriptors_base[0][0] |
                     ((uint16_t)string_descriptors_base[0][1] << 8);
      len = 1;
    } else if (idx <= 3) {
      len = strnlen(string_descriptors_base[idx], 31);
      for (int i = 0; i < len; i++) {
        wchar_str[i + 1] = string_descriptors_base[idx][i];
      }
    }
    wchar_str[0] = (TUSB_DESC_STRING << 8) | (2 * len + 2);
  }
}

static usb_descriptor_buffers_t pio_desc = {
  .device = (uint8_t *)&pio_desc_device,
  .config = pio_desc_configuration,
  .hid_report = report_desc,
  .string = str_desc
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
#define KEYEMU_QUEUE_LEN 64
static uint16_t key_queue[KEYEMU_QUEUE_LEN];
static uint16_t key_queue_head = 0;
static uint16_t key_queue_tail = 0;

static bool key_queue_is_empty(void) {
  return key_queue_head == key_queue_tail;
}

static size_t key_queue_free_space(void) {
  if (key_queue_head >= key_queue_tail) {
    return KEYEMU_QUEUE_LEN - (key_queue_head - key_queue_tail) - 1;
  }
  return (key_queue_tail - key_queue_head) - 1;
}

static bool key_queue_push(uint16_t packed) {
  if (key_queue_free_space() == 0) {
    return false;
  }
  key_queue[key_queue_head] = packed;
  key_queue_head = (uint16_t)((key_queue_head + 1) % KEYEMU_QUEUE_LEN);
  return true;
}

static bool key_queue_pop(uint16_t *out) {
  if (key_queue_is_empty()) {
    return false;
  }
  *out = key_queue[key_queue_tail];
  key_queue_tail = (uint16_t)((key_queue_tail + 1) % KEYEMU_QUEUE_LEN);
  return true;
}

static void pio_send_key(const hid_key_t *key) {
  if (pio_usb_device == NULL || pio_hid_ep == NULL) {
    return;
  }

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
// Core1: PIO USB device task
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

  init_string_desc();
  pio_usb_device = pio_usb_device_init(&pio_cfg, &pio_desc);
  if (pio_usb_device == NULL) {
    LOG_INFO("PIO USB init failed");
    while (true) {
      sleep_ms(1000);
    }
  }
  pio_hid_ep = pio_usb_get_endpoint(pio_usb_device, 1);
  if (pio_hid_ep == NULL) {
    LOG_INFO("PIO USB HID endpoint missing");
    while (true) {
      sleep_ms(1000);
    }
  }
  LOG_INFO("PIO USB HID initialized");

  while (true) {
    pio_usb_device_task();

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
// Core0: CDC (native USB) -> send HID via FIFO
// --------------------------------------------------------------------

int main(void) {
  set_sys_clock_khz(120000, true);

  // Initialize logging before launching core1 to avoid race conditions
  keyemu_log_init();

  multicore_reset_core1();
  multicore_launch_core1(core1_main);

  sleep_ms(100);
  tud_init(0);
  LOG_INFO("keyemu boot");
  LOG_INFO("CDC device ready");

  while (true) {
    tud_task();
    keyemu_log_flush();
    static bool was_connected = false;
    static bool have_keycode = false;
    static uint8_t pending_keycode = 0;
    static uint32_t dropped_queue = 0;
    static absolute_time_t last_rx_time;
    static bool last_rx_time_valid = false;

    // Drain queued packets into the multicore FIFO when space is available.
    while (multicore_fifo_wready()) {
      uint16_t packed;
      if (!key_queue_pop(&packed)) {
        break;
      }
      multicore_fifo_push_blocking((uint32_t)packed);
    }

    bool connected = tud_cdc_connected();
    if (was_connected && !connected) {
      // Drop partial packets on disconnect to avoid stale pairing later.
      have_keycode = false;
      pending_keycode = 0;
      last_rx_time_valid = false;
    }
    was_connected = connected;
    if (have_keycode && last_rx_time_valid) {
      int64_t age_us = absolute_time_diff_us(last_rx_time, get_absolute_time());
      if (age_us > 200000) {
        // If a second byte never arrives, discard the pending keycode.
        have_keycode = false;
        pending_keycode = 0;
        last_rx_time_valid = false;
      }
    }

    // Only read from CDC when the queue has room for more packets.
    size_t free_packets = key_queue_free_space();
    size_t max_bytes = free_packets * 2;
    if (have_keycode) {
      // Allow one extra byte to complete a pending keycode.
      if (max_bytes == 0) {
        continue;
      }
      max_bytes += 1;
    }

    if (tud_cdc_available()) {
      uint8_t buf[64];
      size_t read_len = sizeof(buf);
      if (max_bytes < read_len) {
        read_len = max_bytes;
      }
      if (read_len == 0) {
        continue;
      }
      uint32_t count = tud_cdc_read(buf, (uint32_t)read_len);
      if (count > 0) {
        LOG_DEBUG("CDC RX data");
        last_rx_time = get_absolute_time();
        last_rx_time_valid = true;
      }

      for (uint32_t i = 0; i < count; i++) {
        if (!have_keycode) {
          pending_keycode = buf[i];
          have_keycode = true;
          continue;
        }

        uint8_t modifier = buf[i];
        uint16_t packed = (uint16_t)pending_keycode | ((uint16_t)modifier << 8);
        if (pending_keycode != 0) {
          if (!key_queue_push(packed)) {
            dropped_queue++;
            if ((dropped_queue & 0x3F) == 1) {
              // Throttle noisy logs while still indicating drops.
              LOG_DEBUG("CDC RX drop: queue full");
            }
          }
        }
        have_keycode = false;
      }
    }
  }

  return 0;
}
