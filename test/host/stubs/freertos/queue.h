#pragma once
#include <stddef.h>
#include <stdint.h>
typedef void* QueueHandle_t;
#define pdTRUE 1
#define pdFALSE 0
QueueHandle_t xQueueCreate(size_t depth, size_t item);
int xQueueSend(QueueHandle_t, const void*, uint32_t);
int xQueueReceive(QueueHandle_t, void*, uint32_t);
size_t uxQueueMessagesWaiting(QueueHandle_t);
