#pragma once
typedef void* SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffu
extern int g_lock_depth;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return (SemaphoreHandle_t)1; }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t, unsigned) { g_lock_depth++; return 1; }
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t) { g_lock_depth--; return 1; }
