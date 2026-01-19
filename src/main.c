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
    keyemu_log_write(buf, (size_t)len);
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
  pio_hid_ep = pio_usb_get_endpoint(pio_usb_device, 1);
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
    static uint32_t dropped_fifo = 0;

    bool connected = tud_cdc_connected();
    if (was_connected && !connected) {
      have_keycode = false;
      pending_keycode = 0;
    }
    was_connected = connected;

    if (tud_cdc_available()) {
      uint8_t buf[64];
      uint32_t count = tud_cdc_read(buf, sizeof(buf));
      if (count > 0) {
        LOG_DEBUG("CDC RX data");
      }

      for (uint32_t i = 0; i < count; i++) {
        if (!have_keycode) {
          pending_keycode = buf[i];
          have_keycode = true;
          continue;
        }

        uint8_t modifier = buf[i];
        uint32_t packed = (uint32_t)pending_keycode | ((uint32_t)modifier << 8);
        if (pending_keycode != 0) {
          if (multicore_fifo_wready()) {
            multicore_fifo_push_blocking(packed);
          } else {
            dropped_fifo++;
            if ((dropped_fifo & 0x3F) == 1) {
              LOG_DEBUG("CDC RX drop: FIFO full");
            }
          }
        }
        have_keycode = false;
      }
    }
  }

  return 0;
}
