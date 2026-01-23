#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pico/stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PUSBKB_DEBUG
#define PUSBKB_DEBUG 1
#endif

void log_write(const char *data, size_t len);
void log_flush(void);

// Logging helpers
void log_write_line(const char *level, const char *message);
void log_write_hex2(const char *prefix, uint8_t a, uint8_t b);

#define LOG_INFO(msg) log_write_line("INFO: ", (msg))
#define LOG_WARN(msg) log_write_line("WARN: ", (msg))
#define LOG_ERROR(msg) log_write_line("ERROR: ", (msg))
#if PUSBKB_DEBUG
#define LOG_DEBUG(msg) log_write_line("DEBUG: ", (msg))
#define LOG_DEBUG_PKT(a, b) log_write_hex2("DEBUG: rx ", (a), (b))
#else
#define LOG_DEBUG(msg) do {} while (0)
#define LOG_DEBUG_PKT(a, b) do {} while (0)
#endif

// TinyUSB debug printf hook (used by CFG_TUSB_DEBUG_PRINTF)
int log_tusb_debug_printf(const char *format, ...);

#ifdef __cplusplus
}
#endif
