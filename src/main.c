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
#include "log.h"
#include "tusb.h"

// --------------------------------------------------------------------
// Watchdog configuration
// --------------------------------------------------------------------

// Watchdog timeout in milliseconds. Main loop should iterate in milliseconds.
#define WATCHDOG_TIMEOUT_MS 8000

// --------------------------------------------------------------------
// UART protocol -> HID mapping
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
  bool have_keycode;
  uint8_t pending_keycode;
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
  if (state->have_keycode && state->last_rx_time_valid) {
    int64_t age_us = absolute_time_diff_us(state->last_rx_time,
                                           get_absolute_time());
    if (age_us > 200000) {
      // Drop an incomplete packet if the modifier byte never arrives.
      state->have_keycode = false;
      state->pending_keycode = 0;
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
    if (!state->have_keycode) {
      state->pending_keycode = byte;
      state->have_keycode = true;
      continue;
    }

    uint8_t modifier = byte;
    uint16_t packed = (uint16_t)state->pending_keycode |
                      ((uint16_t)modifier << 8);
    if (state->pending_keycode != 0) {
      if (!key_queue_push(packed)) {
        state->dropped_queue++;
        if ((state->dropped_queue & 0x3F) == 1) {
          LOG_DEBUG("UART RX drop: queue full");
        }
      }
    }
    state->have_keycode = false;
  }
}

static void hid_send_press_release(const hid_key_t *key, uint8_t *stage) {
  if (!tud_hid_ready()) {
    return;
  }
  if (*stage == 1) {
    uint8_t keycodes[6] = {0};
    keycodes[0] = key->keycode;
    tud_hid_keyboard_report(0, key->modifier, keycodes);
    *stage = 2;
  } else if (*stage == 2) {
    uint8_t empty[6] = {0};
    tud_hid_keyboard_report(0, 0, empty);
    *stage = 0;
  }
}

static void hid_queue_task(void) {
  static hid_key_t pending_key = {0};
  static uint8_t pending_stage = 0; // 0 = idle, 1 = send press, 2 = send release

  if (pending_stage != 0) {
    hid_send_press_release(&pending_key, &pending_stage);
    return;
  }

  uint16_t packed;
  if (key_queue_pop(&packed)) {
    pending_key.keycode = (uint8_t)(packed & 0xFF);
    pending_key.modifier = (uint8_t)((packed >> 8) & 0xFF);
    if (pending_key.keycode != 0) {
      LOG_DEBUG_PKT(pending_key.keycode, pending_key.modifier);
      pending_stage = 1;
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
  if (!tud_mounted() || !tud_hid_ready()) {
    return;
  }
  if (!time_reached(next_time)) {
    return;
  }

  uint8_t keycodes[6] = {0};
  keycodes[0] = HID_KEY_A;
  if (stage == 0) {
    tud_hid_keyboard_report(0, KEYBOARD_MODIFIER_LEFTSHIFT, keycodes);
    stage = 1;
    next_time = make_timeout_time_ms(5);
  } else {
    uint8_t empty[6] = {0};
    tud_hid_keyboard_report(0, 0, empty);
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
  log_tusb_debug_printf("TinyUSB debug level %d\r\n", CFG_TUSB_DEBUG);
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
