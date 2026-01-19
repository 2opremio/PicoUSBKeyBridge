/*
 * CDC log buffer with flush to TinyUSB CDC TX.
 *
 * Note: logs are only flushed when the host asserts DTR (tud_cdc_connected()).
 * This avoids sending into a closed port where the host may drop bytes.
 */

#include "keyemu_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/sync.h"
#include "tusb.h"

#define KEYEMU_LOG_BUFFER_SIZE 4096

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

void keyemu_log_flush(void) {
  if (!log_initialized || !tud_cdc_connected()) {
    return;
  }

  uint32_t save = spin_lock_blocking(log_lock);
  size_t dropped = log_dropped_bytes;
  spin_unlock(log_lock, save);

  if (dropped > 0) {
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
      return;
    }
    tud_cdc_write(overflow_msg, (uint32_t)overflow_len);
    save = spin_lock_blocking(log_lock);
    log_dropped_bytes = 0;
    spin_unlock(log_lock, save);
  }

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
    save = spin_lock_blocking(log_lock);
    size_t count = log_pop_chunk(chunk, max_len);
    spin_unlock(log_lock, save);

    if (count == 0) {
      break;
    }
    tud_cdc_write(chunk, (uint32_t)count);
  }
  tud_cdc_write_flush();
}
