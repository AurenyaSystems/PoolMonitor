#pragma once
#include <stdint.h>
inline void vTaskDelay(uint32_t) {}
typedef void (*TaskFunction_t)(void*);
inline int xTaskCreate(TaskFunction_t, const char*, uint32_t, void*, int, void*) { return 1; }
