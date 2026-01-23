/*
 * UART log output (stdio).
 */

#include "log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdio.h"

static int log_write_line_v(const char *level, const char *format, va_list args) {
  if (format == NULL) {
    return 0;
  }
  char buffer[256];
  int len = vsnprintf(buffer, sizeof(buffer), format, args);
  if (len <= 0) {
    return len;
  }
  size_t write_len = (size_t)len;
  if (write_len >= sizeof(buffer)) {
    write_len = sizeof(buffer) - 1;
  }
  if (level != NULL && level[0] != '\0') {
    log_write(level, strlen(level));
  }
  log_write(buffer, write_len);
  if (write_len == 0 || buffer[write_len - 1] != '\n') {
    log_write("\r\n", 2);
  }
  return len;
}

void log_write_line(const char *level, const char *format, ...) {
  va_list args;
  va_start(args, format);
  (void)log_write_line_v(level, format, args);
  va_end(args);
}

void log_write(const char *data, size_t len) {
  if (data == NULL || len == 0) {
    return;
  }
  fwrite(data, 1, len, stdout);
  fflush(stdout);
}

void log_flush(void) {
  fflush(stdout);
}

// TinyUSB debug printf hook (CFG_TUSB_DEBUG_PRINTF).
int log_tusb_debug_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int len = log_write_line_v("", format, args);
  va_end(args);
  return len;
}
