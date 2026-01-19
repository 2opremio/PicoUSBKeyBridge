#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KEYEMU_DEBUG
#define KEYEMU_DEBUG 1
#endif

void keyemu_log_init(void);
void keyemu_log_write(const char *data, size_t len);
void keyemu_log_flush(void);

// TinyUSB debug printf hook (used by CFG_TUSB_DEBUG_PRINTF)
int keyemu_tusb_debug_printf(const char *format, ...);

#ifdef __cplusplus
}
#endif
