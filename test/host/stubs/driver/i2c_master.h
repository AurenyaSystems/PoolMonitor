#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct i2c_bus_stub* i2c_master_bus_handle_t;
typedef struct i2c_dev_stub* i2c_master_dev_handle_t;
enum { I2C_ADDR_BIT_LEN_7 = 0 };
struct i2c_device_config_t { int dev_addr_length; uint16_t device_address; uint32_t scl_speed_hz; uint32_t scl_wait_us; struct { uint32_t disable_ack_check:1; } flags; };
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t*, i2c_master_dev_handle_t*);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t*, size_t, int);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t, uint8_t*, size_t, int);
