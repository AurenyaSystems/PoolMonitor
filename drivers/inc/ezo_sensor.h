#ifndef EZO_SENSOR_H
#define EZO_SENSOR_H

#include <stdint.h>
#include <stddef.h>
#include <cmath>                    // for NAN
#include "driver/i2c_master.h"      // for i2c_master_bus_handle_t

/**
 * @brief Atlas Scientific EZO stamp (pH or ORP) over I2C.
 *
 * Both stamps speak the same protocol: write an ASCII command, wait the
 * command's processing time, then read back a status byte followed by an ASCII
 * reply. The stamp owns the analog front end, so the ESP32 only parses text.
 *
 * Stamps ship in UART mode and must be switched to I2C before use. Default
 * addresses: pH 0x63, ORP 0x62.
 */
class EzoSensor
{
public:
    struct EzoData {
        float value;            ///< pH units or mV
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
        I2C_ERROR,              ///< no ACK or bus error
        SYNTAX_ERROR,           ///< stamp rejected the command (status byte 2)
        NOT_READY,              ///< still processing after the retry window (status byte 254)
        NO_DATA,                ///< nothing to send (status byte 255)
        PARSE_ERROR,            ///< reply was not in the expected format
        IMPLAUSIBLE_READING     ///< parsed, but outside the stamp's measurement range
    };

private:
    i2c_master_bus_handle_t _bus;
    i2c_master_dev_handle_t _dev;
    uint8_t _address;
    float _min;                 ///< plausibility range for readings
    float _max;
    Status _status;
    EzoData _data;              ///< last good reading
    int _consecutive_failures;

    constexpr static const char* TAG = "[EZO]";
    constexpr static uint8_t kFailedThreshold = 5;

    /**
     * @brief Send a command, wait, and read the reply.
     * @param cmd ASCII command, e.g. "R" or "Cal,mid,7.00"
     * @param wait_ms Processing time from the datasheet for this command
     * @param reply Receives the reply text without the status byte (may be nullptr)
     * @param reply_len Size of reply
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode transact(const char* cmd, uint32_t wait_ms, char* reply, size_t reply_len);

    ReturnCode fail(ReturnCode rc);

public:
    /**
     * @brief Constructor for the EzoSensor class
     * @param bus I2C master bus the stamp is on
     * @param address 7-bit I2C address
     * @param min Lowest plausible reading (pH 0, ORP -1019.9 mV)
     * @param max Highest plausible reading (pH 14, ORP 1019.9 mV)
     */
    EzoSensor(i2c_master_bus_handle_t bus, uint8_t address, float min, float max);

    /**
     * @brief Attach the stamp to the bus and confirm it answers the info command.
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode init();

    /**
     * @brief Take one reading ("R", ~900 ms).
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode sample();

    /**
     * @brief Set the temperature used for compensation ("T,<degC>"). pH only.
     *
     * The stamp keeps it until changed or power-cycled; no Nernst math happens
     * in firmware.
     *
     * @param celsius Water temperature
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode set_temperature(float celsius);

    /**
     * @brief Send a calibration command ("Cal,<args>", ~900 ms).
     *
     * pH: "mid,7.00" first (it clears the other points), then "low,4.00",
     * "high,10.00". ORP: "225". Both: "clear".
     *
     * @param args Everything after "Cal,"
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode calibrate(const char* args);

    /**
     * @brief Ask how many calibration points are stored ("Cal,?").
     * @param points Receives the count (0 = uncalibrated)
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode calibration_points(int& points);

    EzoData read_data() const { return _data; }
    Status get_status() const { return _status; }
    int get_consecutive_failures() const { return _consecutive_failures; }
    uint8_t get_address() const { return _address; }

    EzoSensor(const EzoSensor&) = delete;
    EzoSensor& operator=(const EzoSensor&) = delete;
};

#endif // EZO_SENSOR_H
