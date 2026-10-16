#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
enum { NVS_READWRITE = 1 };
#define ESP_ERR_NVS_NOT_FOUND 0x1102
esp_err_t nvs_open(const char*, int, nvs_handle_t*);
esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*);
esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
