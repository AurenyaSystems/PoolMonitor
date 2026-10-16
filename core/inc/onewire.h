#ifndef ONEWIRE_H
#define ONEWIRE_H

#include <stdint.h>         // for uint8_t, uint64_t
#include "driver/gpio.h"    // for gpio_num_t
#include "esp_err.h"        // for esp_err_t

/**
 * @brief Bit-banged 1-Wire bus master on a single GPIO.
 *
 * One instance per GPIO; several devices can share the bus and are told apart
 * by their 64-bit ROM ID. The ROM ID is stored as it comes off the wire: family
 * code in the low byte, CRC in the high byte.
 *
 * Needs an external 4.7k pull-up to 3.3V. The internal pull-up is too weak on
 * its own. Parasitic power is not supported.
 */
class OneWire
{
public:
    /**
     * @brief Constructor for the OneWire class
     * @param pin GPIO the bus data line is connected to
     */
    OneWire(gpio_num_t pin);

    /**
     * @brief Configure the GPIO as open-drain input/output and release the bus.
     * @return ESP_OK if successful, otherwise the gpio_config error
     */
    esp_err_t init();

    /**
     * @brief Send a reset pulse and check for a presence pulse.
     * @return true if at least one device answered
     */
    bool reset();

    /**
     * @brief Reset the bus and address a single device (Match ROM).
     *
     * Any function command written after this goes only to that device.
     *
     * @param rom ROM ID of the device to address
     * @return true if a device answered the reset, false if the bus is empty
     */
    bool select(uint64_t rom);

    /**
     * @brief Find the ROM IDs of all devices on the bus (Search ROM).
     * @param roms Array that receives the ROM IDs found
     * @param max_roms Size of the roms array
     * @return Number of devices found (0 if none or on a bus error)
     */
    int search(uint64_t* roms, int max_roms);

    void write_byte(uint8_t byte);
    uint8_t read_byte();

    /**
     * @brief Dallas/Maxim CRC-8, as used for ROM IDs and DS18B20 scratchpads.
     * @param data Bytes to checksum
     * @param len Number of bytes
     * @return The CRC. Running it over data that includes its own CRC byte gives 0.
     */
    static uint8_t crc8(const uint8_t* data, int len);

    gpio_num_t get_pin() const { return _pin; }

    OneWire(const OneWire&) = delete;
    OneWire& operator=(const OneWire&) = delete;

private:
    gpio_num_t _pin;            ///< GPIO of the bus data line

    void write_bit(bool bit);
    bool read_bit();
};

#endif // ONEWIRE_H
