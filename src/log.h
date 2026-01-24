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
void log_write_line(const char *level, const char *format, ...);

#define LOG_INFO(...) log_write_line("INFO: ", __VA_ARGS__)
#define LOG_WARN(...) log_write_line("WARN: ", __VA_ARGS__)
#define LOG_ERROR(...) log_write_line("ERROR: ", __VA_ARGS__)
#if PUSBKB_DEBUG
#define LOG_DEBUG(...) log_write_line("DEBUG: ", __VA_ARGS__)
#else
#define LOG_DEBUG(...) do {} while (0)
#endif

// TinyUSB debug printf hook (used by CFG_TUSB_DEBUG_PRINTF)
int log_tusb_debug_printf(const char *format, ...);

#ifdef __cplusplus
}
#endif
