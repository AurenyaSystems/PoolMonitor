#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
#define GPIO_NUM_4 4
#define GPIO_NUM_27 27
enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2, GPIO_MODE_INPUT_OUTPUT_OD = 7, GPIO_PULLUP_ENABLE = 1, GPIO_PULLUP_DISABLE = 0, GPIO_PULLDOWN_DISABLE = 0, GPIO_INTR_DISABLE = 0 };
struct gpio_config_t { uint64_t pin_bit_mask; int mode; int pull_up_en; int pull_down_en; int intr_type; };
int gpio_config(const gpio_config_t*);
int gpio_set_level(gpio_num_t, uint32_t);
int gpio_get_level(gpio_num_t);
inline int gpio_set_direction(gpio_num_t, int) { return 0; }
