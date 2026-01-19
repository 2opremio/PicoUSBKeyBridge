/*
 * CDC log buffer with flush to TinyUSB CDC TX.
 *
 * Note: logs are only flushed when the host asserts DTR (tud_cdc_connected()).
 * This avoids sending into a closed port where the host may drop bytes.
 */

#include "keyemu_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#include "hardware/sync.h"
#include "tusb.h"

// RP2040 has 264 KB SRAM (~256 KiB); this log buffer consumes 8 KB.
#define KEYEMU_LOG_BUFFER_SIZE 8192

static uint8_t log_buffer[KEYEMU_LOG_BUFFER_SIZE];
static size_t log_head = 0;
static size_t log_tail = 0;
static size_t log_dropped_bytes = 0;
static spin_lock_t *log_lock = NULL;
static bool log_initialized = false;

void keyemu_log_init(void) {
  if (!log_initialized) {
    int lock_num = spin_lock_claim_unused(true);
    log_lock = spin_lock_instance(lock_num);
    log_initialized = true;
  }
}

// Bytes of free space left in the ring buffer (one slot unused).
static size_t log_free_space(void) {
  if (log_head >= log_tail) {
    return KEYEMU_LOG_BUFFER_SIZE - (log_head - log_tail) - 1;
  }
  return (log_tail - log_head) - 1;
}

void keyemu_log_write(const char *data, size_t len) {
  if (data == NULL || len == 0 || !log_initialized) {
    return;
  }

  uint32_t save = spin_lock_blocking(log_lock);

  size_t free_space = log_free_space();
  if (len > free_space) {
    log_dropped_bytes += (len - free_space);
    len = free_space;
  }

  for (size_t i = 0; i < len; i++) {
    log_buffer[log_head] = (uint8_t)data[i];
    log_head = (log_head + 1) % KEYEMU_LOG_BUFFER_SIZE;
  }

  spin_unlock(log_lock, save);
}

// Pop up to max_len bytes from the ring buffer into out.
static size_t log_pop_chunk(uint8_t *out, size_t max_len) {
  if (log_head == log_tail) {
    return 0;
  }

  size_t count = 0;
  while (log_tail != log_head && count < max_len) {
    out[count++] = log_buffer[log_tail];
    log_tail = (log_tail + 1) % KEYEMU_LOG_BUFFER_SIZE;
  }
  return count;
}

static bool log_cdc_ready(void) {
  // Only flush when CDC is connected (host asserted DTR).
  return log_initialized && tud_cdc_connected();
}

static size_t log_get_dropped_bytes(void) {
  uint32_t save = spin_lock_blocking(log_lock);
  size_t dropped = log_dropped_bytes;
  spin_unlock(log_lock, save);
  return dropped;
}

static void log_clear_dropped_bytes(void) {
  uint32_t save = spin_lock_blocking(log_lock);
  log_dropped_bytes = 0;
  spin_unlock(log_lock, save);
}

static bool log_emit_overflow_warning(size_t dropped) {
  if (dropped == 0) {
    return true;
  }

  char overflow_msg[64];
  int overflow_len = snprintf(overflow_msg, sizeof(overflow_msg),
                              "WARN: log buffer overflow (%u bytes dropped)\r\n",
                              (unsigned)dropped);
  if (overflow_len <= 0) {
    overflow_len = 0;
  } else if ((size_t)overflow_len >= sizeof(overflow_msg)) {
    overflow_len = (int)(sizeof(overflow_msg) - 1);
  }
  // Avoid dropping the overflow warning: only write when the CDC TX buffer can accept it.
  if (tud_cdc_write_available() < (uint32_t)overflow_len) {
    return false;
  }
  tud_cdc_write(overflow_msg, (uint32_t)overflow_len);
  log_clear_dropped_bytes();
  return true;
}

static void log_flush_available_chunks(void) {
  uint8_t chunk[64];
  while (true) {
    uint32_t available = tud_cdc_write_available();
    if (available == 0) {
      break;
    }
    // Only pop as much as the CDC TX buffer can accept to avoid losing logs.
    size_t max_len = sizeof(chunk);
    if (available < max_len) {
      max_len = (size_t)available;
    }
    uint32_t save = spin_lock_blocking(log_lock);
    size_t count = log_pop_chunk(chunk, max_len);
    spin_unlock(log_lock, save);

    if (count == 0) {
      break;
    }
    tud_cdc_write(chunk, (uint32_t)count);
  }
}

void keyemu_log_flush(void) {
  if (!log_cdc_ready()) {
    return;
  }

  size_t dropped = log_get_dropped_bytes();
  log_flush_available_chunks();
  if (!log_emit_overflow_warning(dropped)) {
    return;
  }
  tud_cdc_write_flush();
}

// TinyUSB debug printf hook (CFG_TUSB_DEBUG_PRINTF).
int keyemu_tusb_debug_printf(const char *format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  if (len <= 0) {
    return len;
  }

  if ((size_t)len >= sizeof(buffer)) {
    len = (int)(sizeof(buffer) - 1);
  }

  keyemu_log_write(buffer, (size_t)len);
  return len;
}
