#include "dht_sensor.h"
#include <stdint.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <cstring>
#include "esp_attr.h"

#include "freertos/FreeRTOS.h"

namespace {
    portMUX_TYPE dht_mux = portMUX_INITIALIZER_UNLOCKED;
}

DhtSensor::DhtSensor(gpio_num_t pin, dht_type_t type)
{
    _pin = pin;
    _type = type;
    _status = Status::UNINITIALIZED;
    _last_read_time = 0;
    _data = {NAN, NAN, 0};
    _consecutive_failures = 0;
}

esp_err_t DhtSensor::init()
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << _pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    
    if (err != ESP_OK) {
        _status = Status::FAILED;
        return err;
    }
    _status = Status::READY;
    return ESP_OK;
}

DhtSensor::ReturnCode DhtSensor::sample()
{
    if (_status == Status::UNINITIALIZED) {
        return ReturnCode::NOT_INITIALIZED;
    }

    int64_t current_time = esp_timer_get_time();
    if (current_time - _last_read_time < 2000000) { // 2 seconds
        return ReturnCode::SENSOR_NOT_READY;
    }

    uint8_t data[DHT_DATA_LENGTH_BYTES];
    _last_read_time = current_time;
    ReturnCode ret = read(data);
    if (ret != ReturnCode::DHT_OK) {
        if (++_consecutive_failures >= kFailedThreshold) {
            _status = Status::FAILED;
        } else {
            _status = Status::DEGRADED;
        }
        return ret;
    }

    DhtData candidate {};
    decode(data, candidate);
    candidate.timestamp = current_time;

    if (!is_plausible(candidate)) 
    {
        if (++_consecutive_failures >= kFailedThreshold) {
            _status = Status::FAILED;
        } else {
            _status = Status::DEGRADED;
        }
        return ReturnCode::IMPLAUSIBLE_READING;
    }


    _consecutive_failures = 0;
    _data = candidate;
    _status = Status::READY;

    return ReturnCode::DHT_OK;
}


DhtSensor::ReturnCode IRAM_ATTR DhtSensor::read(uint8_t* data)
{
    memset(data, 0, DHT_DATA_LENGTH_BYTES);

    // Start signal. Deliberately outside the critical section: it is a
    // millisecond-scale hold and does not need interrupt-level timing.
    gpio_set_level(_pin, 0);
    gpio_set_direction(_pin, GPIO_MODE_OUTPUT);
    esp_rom_delay_us(_type == DHT11 ? 20000 : 1100);
    gpio_set_direction(_pin, GPIO_MODE_INPUT);

    ReturnCode result = ReturnCode::DHT_OK;

    // The response handshake and 40-bit frame are microsecond-sensitive.
    // Roughly 5 ms with interrupts disabled, so the calling task must be
    // pinned to core 1 to keep WiFi (core 0) undisturbed.
    portENTER_CRITICAL(&dht_mux);

    if      (!wait_for_level(_pin, DHT_LOW,  300)) result = ReturnCode::TIMEOUT_NO_RESPONSE;
    else if (!wait_for_level(_pin, DHT_HIGH, 120)) result = ReturnCode::TIMEOUT_RESP_LOW;
    else if (!wait_for_level(_pin, DHT_LOW,  120)) result = ReturnCode::TIMEOUT_RESP_HIGH;
    else
    {
        for (int i = 0; i < DHT_DATA_LENGTH_BITS; i++) {
            if (!wait_for_level(_pin, DHT_HIGH, 100)) { result = ReturnCode::TIMEOUT_BIT_LOW;  break; }
            int64_t start = esp_timer_get_time();
            if (!wait_for_level(_pin, DHT_LOW, 100))  { result = ReturnCode::TIMEOUT_BIT_HIGH; break; }
            int64_t duration = esp_timer_get_time() - start;

            data[i / 8] <<= 1;
            if (duration > 48) {
                data[i / 8] |= 1;
            }
        }
    }

    portEXIT_CRITICAL(&dht_mux);

    if (result != ReturnCode::DHT_OK) return result;

    if (checksum(data) != data[4]) return ReturnCode::CHECKSUM_MISMATCH;

    // An all-zero frame has a valid checksum (0+0+0+0 == 0), so the CRC does
    // not catch it. The DHT emits one on power-up before its first conversion.
    if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0) {
        return ReturnCode::IMPLAUSIBLE_READING;
    }

    return ReturnCode::DHT_OK;
}

bool IRAM_ATTR DhtSensor::wait_for_level(gpio_num_t pin, int level, uint32_t timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(pin) != level) {
        if (esp_timer_get_time() - start > timeout_us) return false;
    }
    return true;
}

uint8_t DhtSensor::checksum(uint8_t* data)
{
    uint8_t sum = (data[0] + data[1] + data[2] + data[3]) & 0xFF;
    return sum;
}

void DhtSensor::decode(uint8_t* data, DhtSensor::DhtData& dht_data) const
{
    switch(_type) {
        case DHT11:
            dht_data.humidity    = data[0];
            dht_data.temperature = data[2];
            break;
        case DHT22:
            dht_data.humidity = ((data[0] << 8) | data[1]) * 0.1f;
            dht_data.temperature = (((data[2] & 0x7F) << 8) | data[3]) * 0.1f;
            if (data[2] & 0x80) {
                dht_data.temperature *= -1;
            }
            break;
    }
}

bool DhtSensor::is_plausible(const DhtData& d) const
{
    if (std::isnan(d.temperature) || std::isnan(d.humidity)) return false;

    float t_min, t_max, h_min, h_max;

    switch (_type) {
        case DHT11:
            t_min = kDht11TempMinC; t_max = kDht11TempMaxC;
            h_min = kDht11HumMin;   h_max = kDht11HumMax;
            break;
        case DHT22:
        default:
            t_min = kDht22TempMinC; t_max = kDht22TempMaxC;
            h_min = kDht22HumMin;   h_max = kDht22HumMax;
            break;
    }

    if (d.temperature < t_min || d.temperature > t_max) return false;
    if (d.humidity    < h_min || d.humidity    > h_max) return false;
    return true;
}