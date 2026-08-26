#ifndef DHT_SENSOR_H
#define DHT_SENSOR_H

#include <stdint.h>         // for uint32_t
#include "driver/gpio.h"    // for gpio_num_t
#include <cmath>            // for NAN


/**
 * @brief Class to interface with DHT11 and DHT22 temperature and humidity sensors.
 */
class DhtSensor 
{
public:
    struct DhtData{
        float temperature;
        float humidity;
        int64_t timestamp;
    };
    enum dht_type_t { DHT11, DHT22 };
    
    enum class Status {
        UNINITIALIZED,
        READY,
        DEGRADED,
        FAILED
    };

    enum class ReturnCode {
        DHT_OK = 0,
        TIMEOUT_NO_RESPONSE,
        TIMEOUT_RESP_LOW,
        TIMEOUT_RESP_HIGH,
        CHECKSUM_MISMATCH,
        TIMEOUT_BIT_LOW,
        TIMEOUT_BIT_HIGH,
        NOT_INITIALIZED,
        SENSOR_NOT_READY,
        IMPLAUSIBLE_READING   ///< passed checksum but outside the sensor's rated range
    };


private:
    
   // class members
    gpio_num_t _pin;            ///< Pin number for the DHT sensor
    dht_type_t _type;           ///< Type of DHT sensor (DHT11 or DHT22)
    Status _status;             ///< Current status of the DHT sensor
    int64_t _last_read_time;    ///< Timestamp of the last successful read
    DhtData _data;              ///< Last read data from the DHT sensor
    int _consecutive_failures;  ///< Count of consecutive failures

    constexpr static uint8_t DHT_DATA_LENGTH_BYTES = 5;
    constexpr static uint8_t DHT_DATA_LENGTH_BITS = 40;
    constexpr static uint8_t DHT_LOW = 0;
    constexpr static uint8_t DHT_HIGH = 1;
    constexpr static uint8_t kFailedThreshold = 5; // Number of consecutive failures before marking sensor as FAILED

    // Datasheet-rated measurement ranges. These are accuracy-spec bounds, not
    // physical limits, so a legitimate reading can sit just outside them.
    constexpr static float kDht11TempMinC =  0.0f;
    constexpr static float kDht11TempMaxC = 50.0f;
    constexpr static float kDht11HumMin   = 20.0f;
    constexpr static float kDht11HumMax   = 90.0f;

    constexpr static float kDht22TempMinC = -40.0f;
    constexpr static float kDht22TempMaxC =  80.0f;
    constexpr static float kDht22HumMin   =   0.0f;
    constexpr static float kDht22HumMax   = 100.0f;


    // private methods
    /**
     * @brief Read the raw data from the DHT sensor. 
     *  This method sends the start signal, waits for the sensor 
     *  response, and reads the 40 bits of data. It also checks for 
     *  timeouts and checksum errors.
     * @param data Pointer to a uint8_t array of size 5 to store 
     *  the raw data read from the sensor.
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode read(uint8_t* data);

    /**
     * @brief Wait for a specific level on the GPIO pin.
     * @param pin The GPIO pin to monitor.
     * @param level The level to wait for.
     * @param timeout_us The timeout in microseconds.
     * @return true if the level is detected within the timeout, false otherwise.
     */
    static bool wait_for_level(gpio_num_t pin, int level, uint32_t timeout_us);

    /**
     * @brief Calculate the checksum of the data read from the sensor.
     * @param data Pointer to a uint8_t array of size 5 containing the raw data.
     * @return The calculated checksum.
     */
    static uint8_t checksum(uint8_t* data);

    /**
     * @brief Decode the raw data read from the sensor into temperature and humidity.
     * @param data Pointer to a uint8_t array of size 5 containing the raw data.
     * @param dht_data Reference to a DhtData struct to store the decoded temperature and humidity.
     */
    void decode(uint8_t* data, DhtData& dht_data) const;

    /**
     * @brief Check a decoded reading against the sensor's rated measurement range.
     *
     * The checksum only proves the frame arrived intact, not that the values
     * are physically meaningful. This catches frames that are well-formed but
     * out of range, which the DHT can emit on power-up or when the line is
     * marginal. Ranges are per-type, taken from the respective datasheets.
     *
     * @param d Decoded reading to validate.
     * @return true if temperature and humidity are both within range and
     *  neither is NaN, false otherwise.
     */
    bool is_plausible(const DhtData& d) const;

public:

    DhtSensor(gpio_num_t pin, dht_type_t type);

    /**
     * Initialize the DHT sensor
     * @brief Configure the GPIO pin and set the sensor status to READY. Needs external resistor.
     * @return ESP_OK if successful, otherwise an error code
     */
    esp_err_t init();

    /**
     * Sample the DHT sensor
     * @brief Enforce the minimum sampling interval, read the sensor data, decode it, and update the status and data members.
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode sample(); 

    /**
     * Get the last read data from the DHT sensor
     * @brief Return the last read data from the DHT sensor. If the sensor is not ready, return NAN for temperature and humidity.
     * @return DhtData struct containing temperature, humidity, and timestamp
     */
    DhtData read_data() const {return _data;};

    /**
     * Get the status of the DHT sensor
     * @brief Return the current status of the DHT sensor.
     * @return Status enum indicating the sensor status
     */
    Status get_status() const {return _status;};

    DhtSensor(const DhtSensor&) = delete;
    DhtSensor& operator=(const DhtSensor&) = delete;
};





#endif // DHT_SENSOR_H
