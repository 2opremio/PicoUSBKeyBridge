/*
 * TinyUSB debug printf hook: send logs to CDC TX.
 */

#include <stdarg.h>
#include <stdio.h>

#include "keyemu_log.h"
#include "tusb.h"

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
