#include "onewire.h"

#include "esp_log.h"
#include "esp_rom_sys.h"    // for esp_rom_delay_us
#include "freertos/FreeRTOS.h"

namespace
{
    constexpr const char* TAG = "[1WIRE]";

    // Standard-speed slot timings (Maxim AN126), in microseconds
    constexpr int RESET_PULSE_US     = 480;     // H: master holds the bus low
    constexpr int PRESENCE_WAIT_US   = 70;      // I: release, then sample presence
    constexpr int PRESENCE_FINISH_US = 410;     // J: rest of the presence window

    constexpr int WRITE1_LOW_US      = 6;       // A
    constexpr int WRITE1_FINISH_US   = 64;      // B
    constexpr int WRITE0_LOW_US      = 60;      // C
    constexpr int WRITE0_FINISH_US   = 10;      // D

    constexpr int READ_INIT_US       = 6;       // A: start the read slot
    constexpr int READ_SAMPLE_US     = 9;       // E: sample well inside the 15 us valid window
    constexpr int READ_END_US        = 55;      // F: rest of the slot

    constexpr uint8_t CMD_MATCH_ROM  = 0x55;
    constexpr uint8_t CMD_SEARCH_ROM = 0xF0;

    // Each slot is a few us of timing that an interrupt or a task switch would
    // corrupt, so it runs with interrupts off. Slots are short (<1 ms for a
    // reset, ~70 us for a bit), and the main task is pinned to core 1 so WiFi
    // on core 0 is unaffected. This also keeps light sleep out of a slot.
    portMUX_TYPE onewire_mux = portMUX_INITIALIZER_UNLOCKED;
}

OneWire::OneWire(gpio_num_t pin):
    _pin(pin)
{
}

esp_err_t OneWire::init()
{
    // Open-drain: level 0 pulls the bus low, level 1 releases it to the
    // pull-up. Input stays enabled so the same pin can read the bus back.
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << _pin,
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) { return err; }

    gpio_set_level(_pin, 1);   // release
    return ESP_OK;
}

bool OneWire::reset()
{
    portENTER_CRITICAL(&onewire_mux);
    gpio_set_level(_pin, 0);
    esp_rom_delay_us(RESET_PULSE_US);
    gpio_set_level(_pin, 1);
    esp_rom_delay_us(PRESENCE_WAIT_US);
    bool presence = (gpio_get_level(_pin) == 0);   // a device holds the bus low
    portEXIT_CRITICAL(&onewire_mux);

    // Outside the critical section: just waiting out the presence window
    esp_rom_delay_us(PRESENCE_FINISH_US);
    return presence;
}

void OneWire::write_bit(bool bit)
{
    portENTER_CRITICAL(&onewire_mux);
    gpio_set_level(_pin, 0);
    esp_rom_delay_us(bit ? WRITE1_LOW_US : WRITE0_LOW_US);
    gpio_set_level(_pin, 1);
    esp_rom_delay_us(bit ? WRITE1_FINISH_US : WRITE0_FINISH_US);
    portEXIT_CRITICAL(&onewire_mux);
}

bool OneWire::read_bit()
{
    portENTER_CRITICAL(&onewire_mux);
    gpio_set_level(_pin, 0);
    esp_rom_delay_us(READ_INIT_US);
    gpio_set_level(_pin, 1);
    esp_rom_delay_us(READ_SAMPLE_US);
    bool bit = gpio_get_level(_pin);   // a device sending 0 keeps the bus low
    esp_rom_delay_us(READ_END_US);
    portEXIT_CRITICAL(&onewire_mux);
    return bit;
}

void OneWire::write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++)
    {
        write_bit(byte & 0x01);   // LSB first
        byte >>= 1;
    }
}

uint8_t OneWire::read_byte()
{
    uint8_t value = 0;
    for (int i = 0; i < 8; i++)
    {
        if (read_bit()) { value |= (1 << i); }   // LSB first
    }
    return value;
}

bool OneWire::select(uint64_t rom)
{
    if (!reset()) { return false; }

    write_byte(CMD_MATCH_ROM);
    for (int i = 0; i < 8; i++)
    {
        write_byte((rom >> (8 * i)) & 0xFF);
    }
    return true;
}

int OneWire::search(uint64_t* roms, int max_roms)
{
    // Maxim AN187. Each pass walks the 64 ROM bits; at every bit where devices
    // disagree (discrepancy) it picks a branch, and the next pass takes the
    // other branch at the deepest one still unexplored.
    int count = 0;
    int last_discrepancy = 0;   // bit (1-64) where the previous pass chose 0; 0 = none left
    uint64_t rom = 0;

    do
    {
        if (!reset()) { break; }   // no devices on the bus
        write_byte(CMD_SEARCH_ROM);

        int last_zero = 0;
        for (int bit = 1; bit <= 64; bit++)
        {
            bool id_bit  = read_bit();
            bool cmp_bit = read_bit();
            if (id_bit && cmp_bit)
            {
                ESP_LOGW(TAG, "search: no device answered at bit %d", bit);
                return count;
            }

            bool dir;
            if (id_bit != cmp_bit)
            {
                dir = id_bit;   // all remaining devices agree on this bit
            }
            else
            {
                // Discrepancy: follow last pass's path above the last branch
                // point, take 1 at it, and 0 at any new one below it
                if (bit < last_discrepancy) { dir = (rom >> (bit - 1)) & 1; }
                else                        { dir = (bit == last_discrepancy); }

                if (!dir) { last_zero = bit; }   // a 1-branch is still left to explore here
            }

            if (dir) { rom |=  (1ULL << (bit - 1)); }
            else     { rom &= ~(1ULL << (bit - 1)); }
            write_bit(dir);
        }
        last_discrepancy = last_zero;

        uint8_t bytes[8];
        for (int i = 0; i < 8; i++) { bytes[i] = (rom >> (8 * i)) & 0xFF; }
        if (crc8(bytes, 8) != 0)
        {
            ESP_LOGW(TAG, "search: ROM CRC mismatch, stopping");
            break;
        }

        roms[count++] = rom;
    } while (last_discrepancy != 0 && count < max_roms);

    return count;
}

uint8_t OneWire::crc8(const uint8_t* data, int len)
{
    // x^8 + x^5 + x^4 + 1, bit-reflected (0x8C), init 0
    uint8_t crc = 0;
    for (int i = 0; i < len; i++)
    {
        uint8_t byte = data[i];
        for (int b = 0; b < 8; b++)
        {
            bool mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) { crc ^= 0x8C; }
            byte >>= 1;
        }
    }
    return crc;
}
