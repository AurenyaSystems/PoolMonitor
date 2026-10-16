#ifndef DS18B20_H
#define DS18B20_H

#include <stdint.h>         // for uint64_t
#include <cmath>            // for NAN
#include "onewire.h"

/**
 * @brief DS18B20 temperature probe on a shared 1-Wire bus, addressed by ROM ID.
 *
 * Externally powered (not parasitic) and left at its factory 12-bit resolution.
 */
class Ds18b20
{
public:
    struct Ds18b20Data {
        float temperature;      ///< degC
        int64_t timestamp;      ///< esp_timer time of the reading, us
    };

    enum class Status {
        UNINITIALIZED,
        READY,
        DEGRADED,
        FAILED
    };

    enum class ReturnCode {
        OK = 0,
        NOT_INITIALIZED,
        NOT_A_DS18B20,          ///< ROM family code is not 0x28
        NO_PRESENCE,            ///< nothing answered the bus reset
        CRC_MISMATCH,
        IMPLAUSIBLE_READING     ///< passed CRC but is a power-on value or out of range
    };

    constexpr static uint8_t FAMILY_CODE = 0x28;

private:
    OneWire& _bus;              ///< Bus the probe is on, shared with other probes
    uint64_t _rom;              ///< ROM ID of this probe
    Status _status;             ///< Current status of the probe
    Ds18b20Data _data;          ///< Last good reading
    int _consecutive_failures;  ///< Count of consecutive failures

    constexpr static const char* TAG = "[DS18B20]";

    constexpr static uint8_t kFailedThreshold = 5;      // consecutive failures before FAILED
    constexpr static uint32_t kConversionMs   = 750;    // 12-bit conversion time (datasheet max)

    // Rated range from the datasheet
    constexpr static float kTempMinC = -55.0f;
    constexpr static float kTempMaxC = 125.0f;

    /**
     * @brief Record a failed sample and update the status.
     * @param rc The failure to record
     * @return rc, so callers can `return fail(...)`
     */
    ReturnCode fail(ReturnCode rc);

public:
    /**
     * @brief Constructor for the Ds18b20 class
     * @param bus 1-Wire bus the probe is on; must already be initialised
     * @param rom ROM ID of the probe, e.g. from OneWire::search()
     */
    Ds18b20(OneWire& bus, uint64_t rom);

    /**
     * @brief Check the ROM is a DS18B20 and that the bus has a device on it.
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode init();

    /**
     * @brief Start a conversion, wait for it, and read the result.
     *
     * Blocks for ~750 ms (the conversion time) in vTaskDelay, so the CPU can
     * light-sleep meanwhile.
     *
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode sample();

    /**
     * @brief Get the last good reading. temperature is NAN until the first good sample.
     * @return Ds18b20Data with temperature and timestamp
     */
    Ds18b20Data read_data() const { return _data; }

    Status get_status() const { return _status; }
    uint64_t get_rom() const { return _rom; }

    Ds18b20(const Ds18b20&) = delete;
    Ds18b20& operator=(const Ds18b20&) = delete;
};

#endif // DS18B20_H
