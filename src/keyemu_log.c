/*
 * CDC log buffer with flush to TinyUSB CDC TX.
 */

#include "keyemu_log.h"

#include <stdbool.h>
#include <stdint.h>

#include "hardware/sync.h"
#include "tusb.h"

#define KEYEMU_LOG_BUFFER_SIZE 4096

static uint8_t log_buffer[KEYEMU_LOG_BUFFER_SIZE];
static size_t log_head = 0;
static size_t log_tail = 0;
static bool log_overflow = false;
static spin_lock_t *log_lock = NULL;

static void log_lock_init(void) {
  if (log_lock == NULL) {
    log_lock = spin_lock_instance(31);
  }
}

static size_t log_free_space(void) {
  if (log_head >= log_tail) {
    return KEYEMU_LOG_BUFFER_SIZE - (log_head - log_tail) - 1;
  }
  return (log_tail - log_head) - 1;
}

void keyemu_log_write(const char *data, size_t len) {
  if (data == NULL || len == 0) {
    return;
  }

  log_lock_init();
  uint32_t save = spin_lock_blocking(log_lock);

  size_t free_space = log_free_space();
  if (len > free_space) {
    log_overflow = true;
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
  if (!tud_cdc_connected()) {
    return;
  }

  log_lock_init();
  uint32_t save = spin_lock_blocking(log_lock);
  bool overflowed = log_overflow;
  log_overflow = false;
  spin_unlock(log_lock, save);

  if (overflowed) {
    static const char overflow_msg[] = "WARN: log buffer overflow\r\n";
    tud_cdc_write(overflow_msg, sizeof(overflow_msg) - 1);
  }

  uint8_t chunk[64];
  while (true) {
    save = spin_lock_blocking(log_lock);
    size_t count = log_pop_chunk(chunk, sizeof(chunk));
    spin_unlock(log_lock, save);

    if (count == 0) {
      break;
    }
    tud_cdc_write(chunk, (uint32_t)count);
  }
  tud_cdc_write_flush();
}
