#include "ezo_sensor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
    constexpr uint32_t I2C_SPEED_HZ     = 100000;   // Atlas recommends 100 kHz
    constexpr int      I2C_TIMEOUT_MS   = 100;

    // Processing times from the datasheets
    constexpr uint32_t READ_WAIT_MS     = 900;      // R
    constexpr uint32_t CAL_WAIT_MS      = 900;      // Cal,...
    constexpr uint32_t SHORT_WAIT_MS    = 300;      // i, T, Cal,?

    // If the stamp says it is still processing, poll a few more times
    constexpr uint32_t RETRY_WAIT_MS    = 100;
    constexpr int      MAX_RETRIES      = 5;

    // First byte of every reply
    constexpr uint8_t  CODE_SUCCESS     = 1;
    constexpr uint8_t  CODE_SYNTAX      = 2;
    constexpr uint8_t  CODE_PENDING     = 254;
    constexpr uint8_t  CODE_NO_DATA     = 255;

    constexpr size_t   REPLY_MAX        = 32;       // status byte + up to 31 chars
}

EzoSensor::EzoSensor(i2c_master_bus_handle_t bus, uint8_t address, float min, float max):
    _bus(bus),
    _dev(nullptr),
    _address(address),
    _min(min),
    _max(max),
    _status(Status::UNINITIALIZED),
    _data{NAN, 0},
    _consecutive_failures(0)
{
}

EzoSensor::ReturnCode EzoSensor::init()
{
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = _address;
    cfg.scl_speed_hz    = I2C_SPEED_HZ;

    if (i2c_master_bus_add_device(_bus, &cfg, &_dev) != ESP_OK)
    {
        ESP_LOGE(TAG, "0x%02X: add device failed", _address);
        return ReturnCode::I2C_ERROR;
    }

    // "i" answers with e.g. "?I,pH,2.16": confirms an EZO is at this address
    // and logs which kind, which catches swapped addresses at bring-up
    char reply[REPLY_MAX];
    ReturnCode rc = transact("i", SHORT_WAIT_MS, reply, sizeof(reply));
    if (rc != ReturnCode::OK) {
        ESP_LOGE(TAG, "0x%02X: no answer to info command, rc=%d", _address, static_cast<int>(rc));
        return rc;
    }
    if (strncmp(reply, "?I,", 3) != 0) {
        ESP_LOGE(TAG, "0x%02X: unexpected info reply '%s'", _address, reply);
        return ReturnCode::PARSE_ERROR;
    }

    ESP_LOGI(TAG, "0x%02X: %s", _address, reply);
    _status = Status::READY;
    return ReturnCode::OK;
}

EzoSensor::ReturnCode EzoSensor::sample()
{
    if (_status == Status::UNINITIALIZED) { return ReturnCode::NOT_INITIALIZED; }

    char reply[REPLY_MAX];
    ReturnCode rc = transact("R", READ_WAIT_MS, reply, sizeof(reply));
    if (rc != ReturnCode::OK) { return fail(rc); }

    char* end = nullptr;
    float value = strtof(reply, &end);
    if (end == reply || *end != '\0') { return fail(ReturnCode::PARSE_ERROR); }

    if (value < _min || value > _max) { return fail(ReturnCode::IMPLAUSIBLE_READING); }

    _consecutive_failures = 0;
    _data = {value, esp_timer_get_time()};
    _status = Status::READY;
    return ReturnCode::OK;
}

EzoSensor::ReturnCode EzoSensor::set_temperature(float celsius)
{
    if (_status == Status::UNINITIALIZED) { return ReturnCode::NOT_INITIALIZED; }

    char cmd[16];
    snprintf(cmd, sizeof(cmd), "T,%.2f", celsius);
    return transact(cmd, SHORT_WAIT_MS, nullptr, 0);
}

EzoSensor::ReturnCode EzoSensor::calibrate(const char* args)
{
    if (_status == Status::UNINITIALIZED) { return ReturnCode::NOT_INITIALIZED; }

    char cmd[24];
    snprintf(cmd, sizeof(cmd), "Cal,%s", args);
    ReturnCode rc = transact(cmd, CAL_WAIT_MS, nullptr, 0);
    if (rc == ReturnCode::OK) {
        ESP_LOGI(TAG, "0x%02X: calibrated '%s'", _address, cmd);
    } else {
        ESP_LOGE(TAG, "0x%02X: '%s' failed, rc=%d", _address, cmd, static_cast<int>(rc));
    }
    return rc;
}

EzoSensor::ReturnCode EzoSensor::calibration_points(int& points)
{
    if (_status == Status::UNINITIALIZED) { return ReturnCode::NOT_INITIALIZED; }

    char reply[REPLY_MAX];
    ReturnCode rc = transact("Cal,?", SHORT_WAIT_MS, reply, sizeof(reply));
    if (rc != ReturnCode::OK) { return rc; }

    // "?CAL,2"
    if (sscanf(reply, "?CAL,%d", &points) != 1) { return ReturnCode::PARSE_ERROR; }
    return ReturnCode::OK;
}

EzoSensor::ReturnCode EzoSensor::transact(const char* cmd, uint32_t wait_ms, char* reply, size_t reply_len)
{
    if (i2c_master_transmit(_dev, reinterpret_cast<const uint8_t*>(cmd), strlen(cmd), I2C_TIMEOUT_MS) != ESP_OK)
    {
        return ReturnCode::I2C_ERROR;
    }

    vTaskDelay(pdMS_TO_TICKS(wait_ms));

    uint8_t buf[REPLY_MAX];
    for (int attempt = 0; ; attempt++)
    {
        memset(buf, 0, sizeof(buf));
        if (i2c_master_receive(_dev, buf, sizeof(buf), I2C_TIMEOUT_MS) != ESP_OK)
        {
            return ReturnCode::I2C_ERROR;
        }

        if (buf[0] != CODE_PENDING) { break; }
        if (attempt >= MAX_RETRIES)  { return ReturnCode::NOT_READY; }
        vTaskDelay(pdMS_TO_TICKS(RETRY_WAIT_MS));
    }

    switch (buf[0])
    {
        case CODE_SUCCESS: break;
        case CODE_SYNTAX:  return ReturnCode::SYNTAX_ERROR;
        case CODE_NO_DATA: return ReturnCode::NO_DATA;
        default:           return ReturnCode::PARSE_ERROR;
    }

    if (reply && reply_len > 0)
    {
        // Reply text follows the status byte and ends with a NUL; the buffer
        // was zeroed, so the copy is terminated even if the NUL was cut off
        size_t n = strnlen(reinterpret_cast<const char*>(buf + 1), sizeof(buf) - 1);
        if (n >= reply_len) { n = reply_len - 1; }
        memcpy(reply, buf + 1, n);
        reply[n] = '\0';
    }
    return ReturnCode::OK;
}

EzoSensor::ReturnCode EzoSensor::fail(ReturnCode rc)
{
    if (++_consecutive_failures >= kFailedThreshold) {
        _status = Status::FAILED;
    } else {
        _status = Status::DEGRADED;
    }
    return rc;
}
