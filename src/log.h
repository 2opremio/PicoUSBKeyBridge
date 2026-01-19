#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PUSBKB_DEBUG
#define PUSBKB_DEBUG 1
#endif

void log_init(void);
void log_write(const char *data, size_t len);
void log_flush(void);

// TinyUSB debug printf hook (used by CFG_TUSB_DEBUG_PRINTF)
int log_tusb_debug_printf(const char *format, ...);

#ifdef __cplusplus
}
#endif
