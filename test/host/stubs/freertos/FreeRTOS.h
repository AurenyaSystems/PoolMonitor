#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
extern int g_critical_depth;
#define portENTER_CRITICAL(m) (g_critical_depth++)
#define portEXIT_CRITICAL(m) (g_critical_depth--)
#define pdMS_TO_TICKS(ms) (ms)
