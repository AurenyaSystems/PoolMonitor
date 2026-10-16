#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_timer* esp_timer_handle_t;
extern int64_t g_fake_time_us;
inline int64_t esp_timer_get_time() { g_fake_time_us += 10; return g_fake_time_us; }
