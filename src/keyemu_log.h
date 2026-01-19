#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KEYEMU_DEBUG
#define KEYEMU_DEBUG 1
#endif

void keyemu_log_write(const char *data, size_t len);
void keyemu_log_flush(void);

#ifdef __cplusplus
}
#endif
