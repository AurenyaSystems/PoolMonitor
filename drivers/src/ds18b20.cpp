#include "ds18b20.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
    constexpr uint8_t CMD_CONVERT_T       = 0x44;
    constexpr uint8_t CMD_READ_SCRATCHPAD = 0xBE;

    constexpr int SCRATCHPAD_BYTES        = 9;      // 8 data bytes + CRC

    // The probe powers up with 85.0 degC in the scratchpad. Reading it back
    // means the conversion never ran (e.g. a brownout reset the probe).
    constexpr int16_t POWER_ON_RAW        = 0x0550;
}

Ds18b20::Ds18b20(OneWire& bus, uint64_t rom):
    _bus(bus),
    _rom(rom),
    _status(Status::UNINITIALIZED),
    _data{NAN, 0},
    _consecutive_failures(0)
{
}

Ds18b20::ReturnCode Ds18b20::init()
{
    if ((_rom & 0xFF) != FAMILY_CODE)
    {
        ESP_LOGE(TAG, "ROM %016llX is not a DS18B20", _rom);
        return ReturnCode::NOT_A_DS18B20;
    }
    if (!_bus.reset())
    {
        ESP_LOGE(TAG, "no device on GPIO %d", _bus.get_pin());
        return ReturnCode::NO_PRESENCE;
    }

    _status = Status::READY;
    return ReturnCode::OK;
}

Ds18b20::ReturnCode Ds18b20::sample()
{
    if (_status == Status::UNINITIALIZED) { return ReturnCode::NOT_INITIALIZED; }

    if (!_bus.select(_rom)) { return fail(ReturnCode::NO_PRESENCE); }
    _bus.write_byte(CMD_CONVERT_T);

    vTaskDelay(pdMS_TO_TICKS(kConversionMs));

    if (!_bus.select(_rom)) { return fail(ReturnCode::NO_PRESENCE); }
    _bus.write_byte(CMD_READ_SCRATCHPAD);

    uint8_t sp[SCRATCHPAD_BYTES];
    for (int i = 0; i < SCRATCHPAD_BYTES; i++) { sp[i] = _bus.read_byte(); }

    if (OneWire::crc8(sp, SCRATCHPAD_BYTES) != 0) { return fail(ReturnCode::CRC_MISMATCH); }

    // An all-zero scratchpad passes the CRC, and so does a bus stuck low. The
    // config register (byte 4) always has its low 5 bits set on a real read.
    if ((sp[4] & 0x1F) != 0x1F) { return fail(ReturnCode::IMPLAUSIBLE_READING); }

    int16_t raw = static_cast<int16_t>((sp[1] << 8) | sp[0]);
    float temperature = raw / 16.0f;   // 12-bit: 1/16 degC per LSB

    if (raw == POWER_ON_RAW || temperature < kTempMinC || temperature > kTempMaxC)
    {
        return fail(ReturnCode::IMPLAUSIBLE_READING);
    }

    _consecutive_failures = 0;
    _data = {temperature, esp_timer_get_time()};
    _status = Status::READY;
    return ReturnCode::OK;
}

Ds18b20::ReturnCode Ds18b20::fail(ReturnCode rc)
{
    if (++_consecutive_failures >= kFailedThreshold) {
        _status = Status::FAILED;
    } else {
        _status = Status::DEGRADED;
    }
    return rc;
}
