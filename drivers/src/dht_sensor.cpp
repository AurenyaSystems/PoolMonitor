#include "dht_sensor.h"
#include <stdint.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <cstring>

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
    _consecutive_failures = 0;

    decode(data, _data);
    _data.timestamp = current_time;
    _status = Status::READY;

    return ReturnCode::DHT_OK;
}


DhtSensor::ReturnCode DhtSensor::read(uint8_t* data)
{
    memset(data, 0, DHT_DATA_LENGTH_BYTES);
    // Send start signal
    gpio_set_level(_pin, 0);
    gpio_set_direction(_pin, GPIO_MODE_OUTPUT);
    esp_rom_delay_us(_type == DHT11 ? 20000 : 1100);
    gpio_set_direction(_pin, GPIO_MODE_INPUT);

    // Wait for sensor response
    if (!wait_for_level(_pin, DHT_LOW, 300)) return ReturnCode::TIMEOUT_NO_RESPONSE;
    if (!wait_for_level(_pin, DHT_HIGH, 120)) return ReturnCode::TIMEOUT_RESP_LOW;
    if (!wait_for_level(_pin, DHT_LOW, 120)) return ReturnCode::TIMEOUT_RESP_HIGH;

    // line is at low
    for (int i =0; i < DHT_DATA_LENGTH_BITS; i++) {
        if (!wait_for_level(_pin, DHT_HIGH, 100)) return ReturnCode::TIMEOUT_BIT_LOW;
        int64_t start = esp_timer_get_time();
        if (!wait_for_level(_pin, DHT_LOW, 100)) return ReturnCode::TIMEOUT_BIT_HIGH;
        int64_t duration = esp_timer_get_time() - start;

        data[i / 8] <<= 1;
        if (duration > 48) {
            data[i / 8] |= 1;
        }
    }

    if (checksum(data) != data[4]) return ReturnCode::CHECKSUM_MISMATCH;
    return ReturnCode::DHT_OK;

}

bool DhtSensor::wait_for_level(gpio_num_t pin, int level, uint32_t timeout_us) 
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