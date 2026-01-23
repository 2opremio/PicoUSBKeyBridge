/*
 * PicoUSBKeyBridge: UART (FTDI) -> HID keyboard (TinyUSB over USB-C)
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/stdio_uart.h"

#include "class/hid/hid_device.h"
#include "hid_reports.h"
#include "log.h"
#include "tusb.h"

// --------------------------------------------------------------------
// Watchdog configuration
// --------------------------------------------------------------------

// Watchdog timeout in milliseconds. Main loop should iterate in milliseconds.
#define WATCHDOG_TIMEOUT_MS 8000

// --------------------------------------------------------------------
// UART protocol -> HID mapping
//
// Packet format (5 bytes):
//   [type] [code_lo] [code_hi] [modifier] [flags]
//
// type byte:
//   - low nibble: 0 = keyboard, 1 = consumer, 2 = vendor
//   - bit 7: set for release, clear for press
//
// Keyboard payload: 16-bit code + modifier byte
// Consumer/vendor payload: 16-bit usage (little-endian)
// --------------------------------------------------------------------

typedef struct {
  uint16_t keycode;
  uint8_t modifier;
  bool apple_fn;
} hid_key_t;

typedef enum {
  RX_MODE_TYPE = 0,
  RX_MODE_CODE_LO,
  RX_MODE_CODE_HI,
  RX_MODE_MODIFIER,
  RX_MODE_FLAGS,
} uart_rx_mode_t;

// Small ring buffer to absorb CDC bursts without blocking USB tasks.
// This lives on core0 and feeds the multicore FIFO opportunistically.
#define PUSBKB_QUEUE_LEN 64
static uint64_t key_queue[PUSBKB_QUEUE_LEN];
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

static bool key_queue_push(uint64_t packed) {
  if (key_queue_free_space() == 0) {
    return false;
  }
  key_queue[key_queue_head] = packed;
  key_queue_head = (uint16_t)((key_queue_head + 1) % PUSBKB_QUEUE_LEN);
  return true;
}

static bool key_queue_pop(uint64_t *out) {
  if (key_queue_is_empty()) {
    return false;
  }
  *out = key_queue[key_queue_tail];
  key_queue_tail = (uint16_t)((key_queue_tail + 1) % PUSBKB_QUEUE_LEN);
  return true;
}

// UART configuration defaults (overridable via compile definitions).
#ifndef PUSBKB_UART_INDEX
#define PUSBKB_UART_INDEX 0
#endif
#ifndef PUSBKB_UART_BAUDRATE
#define PUSBKB_UART_BAUDRATE 115200
#endif
#ifndef PUSBKB_UART_TX_PIN
#define PUSBKB_UART_TX_PIN 0
#endif
#ifndef PUSBKB_UART_RX_PIN
#define PUSBKB_UART_RX_PIN 1
#endif
#ifndef PUSBKB_HID_TEST
#define PUSBKB_HID_TEST 0
#endif

typedef struct {
  uart_rx_mode_t rx_mode;
  uint8_t pending_type;
  uint8_t pending_code_lo;
  uint8_t pending_code_hi;
  uint8_t pending_modifier;
  uint8_t pending_flags;
  uint32_t dropped_queue;
  absolute_time_t last_rx_time;
  bool last_rx_time_valid;
} uart_rx_state_t;

static uart_inst_t *get_uart_instance(void) {
  return (PUSBKB_UART_INDEX == 0) ? uart0 : uart1;
}

static void uart_configure(void) {
  uart_inst_t *uart = get_uart_instance();
  uart_init(uart, PUSBKB_UART_BAUDRATE);
  gpio_set_function(PUSBKB_UART_TX_PIN, GPIO_FUNC_UART);
  gpio_set_function(PUSBKB_UART_RX_PIN, GPIO_FUNC_UART);
  uart_set_format(uart, 8, 1, UART_PARITY_NONE);
  uart_set_hw_flow(uart, false, false);
  uart_set_fifo_enabled(uart, true);
  stdio_uart_init_full(uart, PUSBKB_UART_BAUDRATE,
                       PUSBKB_UART_TX_PIN, PUSBKB_UART_RX_PIN);
}

static void uart_update_state(uart_rx_state_t *state) {
  if (state->rx_mode != RX_MODE_TYPE && state->last_rx_time_valid) {
    int64_t age_us = absolute_time_diff_us(state->last_rx_time,
                                           get_absolute_time());
    if (age_us > 200000) {
      // Drop an incomplete packet if the payload never arrives.
      state->rx_mode = RX_MODE_TYPE;
      state->pending_type = 0;
      state->pending_code_lo = 0;
      state->pending_code_hi = 0;
      state->pending_modifier = 0;
      state->pending_flags = 0;
      state->last_rx_time_valid = false;
    }
  }
}

static void uart_handle_input(uart_rx_state_t *state) {
  uart_inst_t *uart = get_uart_instance();
  while (uart_is_readable(uart)) {
    uint8_t byte = uart_getc(uart);
    state->last_rx_time = get_absolute_time();
    state->last_rx_time_valid = true;
    switch (state->rx_mode) {
      case RX_MODE_TYPE:
        state->pending_type = byte;
        state->rx_mode = RX_MODE_CODE_LO;
        break;
      case RX_MODE_CODE_LO:
        state->pending_code_lo = byte;
        state->rx_mode = RX_MODE_CODE_HI;
        break;
      case RX_MODE_CODE_HI:
        state->pending_code_hi = byte;
        state->rx_mode = RX_MODE_MODIFIER;
        break;
      case RX_MODE_MODIFIER:
        state->pending_modifier = byte;
        state->rx_mode = RX_MODE_FLAGS;
        break;
      case RX_MODE_FLAGS: {
        state->pending_flags = byte;
        uint64_t packed = ((uint64_t)state->pending_type << 32) |
                          ((uint64_t)state->pending_flags << 24) |
                          ((uint64_t)state->pending_modifier << 16) |
                          ((uint64_t)state->pending_code_hi << 8) |
                          (uint64_t)state->pending_code_lo;
        if (!key_queue_push(packed)) {
          state->dropped_queue++;
          if ((state->dropped_queue & 0x3F) == 1) {
            LOG_DEBUG("UART RX drop: queue full");
          }
        }
        state->rx_mode = RX_MODE_TYPE;
        break;
      }
      default:
        state->rx_mode = RX_MODE_TYPE;
        break;
    }
  }
}

static void hid_send_press_release(const hid_key_t *key, uint8_t *stage) {
  if (!tud_hid_n_ready(PUSBKB_HID_ITF_KEYBOARD)) {
    return;
  }
  if (*stage == 1) {
    uint8_t keycodes[6] = {0};
    keycodes[0] = (uint8_t)key->keycode;
    struct __attribute__((packed)) {
      uint8_t modifier;
      uint8_t apple_fn;
      uint8_t keycode[6];
    } report = {
      .modifier = key->modifier,
      .apple_fn = key->apple_fn ? 1 : 0,
    };
    memcpy(report.keycode, keycodes, sizeof(keycodes));
    tud_hid_n_report(PUSBKB_HID_ITF_KEYBOARD, 0, &report, sizeof(report));
    *stage = 2;
  } else if (*stage == 2) {
    struct __attribute__((packed)) {
      uint8_t modifier;
      uint8_t apple_fn;
      uint8_t keycode[6];
    } report = {0};
    tud_hid_n_report(PUSBKB_HID_ITF_KEYBOARD, 0, &report, sizeof(report));
    *stage = 0;
  }
}

static void hid_send_consumer_press_release(uint16_t usage, uint8_t *stage) {
  if (!tud_hid_n_ready(PUSBKB_HID_ITF_AUX)) {
    return;
  }
  if (*stage == 1) {
    tud_hid_n_report(PUSBKB_HID_ITF_AUX, PUSBKB_REPORT_ID_CONSUMER,
                     &usage, sizeof(usage));
    *stage = 2;
  } else if (*stage == 2) {
    uint16_t zero = 0;
    tud_hid_n_report(PUSBKB_HID_ITF_AUX, PUSBKB_REPORT_ID_CONSUMER,
                     &zero, sizeof(zero));
    *stage = 0;
  }
}

static void hid_send_vendor_press_release(uint16_t usage, uint8_t *stage) {
  if (!tud_hid_n_ready(PUSBKB_HID_ITF_AUX)) {
    return;
  }
  if (*stage == 1) {
    tud_hid_n_report(PUSBKB_HID_ITF_AUX, PUSBKB_REPORT_ID_VENDOR,
                     &usage, sizeof(usage));
    *stage = 2;
  } else if (*stage == 2) {
    uint16_t zero = 0;
    tud_hid_n_report(PUSBKB_HID_ITF_AUX, PUSBKB_REPORT_ID_VENDOR,
                     &zero, sizeof(zero));
    *stage = 0;
  }
}

static void hid_queue_task(void) {
  static hid_key_t pending_key = {0};
  static uint8_t pending_stage = 0; // 0 = idle, 1 = send press, 2 = send release
  static pusbkb_pkt_type_t pending_type = PUSBKB_PKT_TYPE_KEYBOARD;
  static uint16_t pending_usage = 0;

  if (pending_stage != 0) {
    if (pending_type == PUSBKB_PKT_TYPE_KEYBOARD) {
      hid_send_press_release(&pending_key, &pending_stage);
    } else if (pending_type == PUSBKB_PKT_TYPE_CONSUMER) {
      hid_send_consumer_press_release(pending_usage, &pending_stage);
    } else if (pending_type == PUSBKB_PKT_TYPE_VENDOR) {
      hid_send_vendor_press_release(pending_usage, &pending_stage);
    } else {
      pending_stage = 0;
    }
    return;
  }

  uint64_t packed;
  if (key_queue_pop(&packed)) {
    uint8_t type_byte = (uint8_t)((packed >> 32) & 0xFF);
    pending_type = (pusbkb_pkt_type_t)(type_byte & PUSBKB_PKT_TYPE_MASK);
    bool is_release = (type_byte & PUSBKB_PKT_FLAG_RELEASE) != 0;

    if (pending_type == PUSBKB_PKT_TYPE_KEYBOARD) {
      pending_key.keycode = (uint16_t)(packed & 0xFFFF);
      pending_key.modifier = (uint8_t)((packed >> 16) & 0xFF);
      pending_key.apple_fn = ((packed >> 24) & PUSBKB_KBD_FLAG_APPLE_FN) != 0;
      if (is_release) {
        struct __attribute__((packed)) {
          uint8_t modifier;
          uint8_t apple_fn;
          uint8_t keycode[6];
        } report = {0};
        tud_hid_n_report(PUSBKB_HID_ITF_KEYBOARD, 0, &report, sizeof(report));
        return;
      }
      LOG_DEBUG_PKT((uint8_t)pending_key.keycode, pending_key.modifier);
      pending_stage = 1;
      return;
    }

    if (pending_type == PUSBKB_PKT_TYPE_CONSUMER ||
        pending_type == PUSBKB_PKT_TYPE_VENDOR) {
      pending_usage = (uint16_t)(packed & 0xFFFF);
      if (is_release) {
        uint16_t zero = 0;
        if (pending_type == PUSBKB_PKT_TYPE_CONSUMER) {
          tud_hid_n_report(PUSBKB_HID_ITF_AUX, PUSBKB_REPORT_ID_CONSUMER,
                           &zero, sizeof(zero));
        } else {
          tud_hid_n_report(PUSBKB_HID_ITF_AUX, PUSBKB_REPORT_ID_VENDOR,
                           &zero, sizeof(zero));
        }
        return;
      }
      pending_stage = 1;
      return;
    }
  }
}

static void hid_test_task(void) {
#if PUSBKB_HID_TEST
  static uint8_t stage = 0;
  static absolute_time_t next_time;
  static bool initialized = false;
  if (!initialized) {
    next_time = make_timeout_time_ms(0);
    initialized = true;
  }
  if (!tud_mounted() || !tud_hid_n_ready(PUSBKB_HID_ITF_KEYBOARD)) {
    return;
  }
  if (!time_reached(next_time)) {
    return;
  }

  if (stage == 0) {
    struct __attribute__((packed)) {
      uint8_t modifier;
      uint8_t apple_fn;
      uint8_t keycode[6];
    } report = {
      .modifier = KEYBOARD_MODIFIER_LEFTSHIFT,
      .apple_fn = 0,
      .keycode = {HID_KEY_A, 0, 0, 0, 0, 0},
    };
    tud_hid_n_report(PUSBKB_HID_ITF_KEYBOARD, 0, &report, sizeof(report));
    stage = 1;
    next_time = make_timeout_time_ms(5);
  } else {
    struct __attribute__((packed)) {
      uint8_t modifier;
      uint8_t apple_fn;
      uint8_t keycode[6];
    } report = {0};
    tud_hid_n_report(PUSBKB_HID_ITF_KEYBOARD, 0, &report, sizeof(report));
    stage = 0;
    next_time = make_timeout_time_ms(1000);
  }
#endif
}

// TinyUSB HID callbacks (not used for this device).
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t* buffer,
                               uint16_t reqlen) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;
  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const* buffer,
                           uint16_t bufsize) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)bufsize;
}

int main(void) {
  set_sys_clock_khz(120000, true);

  // Initialize UART stdio before TinyUSB to capture early logs.
  uart_configure();

  // Check if we rebooted due to watchdog
  bool watchdog_reboot = watchdog_enable_caused_reboot();

  // Initialize the native USB stack (HID on the built-in USB port).
  if (!tud_init(0)) {
    LOG_ERROR("tud_init failed");
  }
  LOG_INFO("TinyUSB debug level %d", CFG_TUSB_DEBUG);
  LOG_INFO("build " PUSBKB_GIT_COMMIT);

  if (watchdog_reboot) {
    LOG_WARN("watchdog triggered reboot");
  }
  LOG_INFO("PicoUSBKeyBridge boot");
#if PUSBKB_HID_TEST
  LOG_INFO("HID test mode enabled");
#endif

  // Enable watchdog with timeout, pause during debug sessions
  watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
  LOG_INFO("watchdog enabled");

  uart_rx_state_t uart_rx_state = {0};

  while (true) {
    watchdog_update();
    tud_task();
    log_flush();
#if PUSBKB_HID_TEST
    hid_test_task();
#else
    uart_update_state(&uart_rx_state);
    uart_handle_input(&uart_rx_state);
    hid_queue_task();
#endif
  }

  return 0;
}
