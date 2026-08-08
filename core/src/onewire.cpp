#include "onewire.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace 
{
    // implementation-only timing constants:
    static constexpr int RESET_PULSE_US     = 480;
    static constexpr int PRESENCE_WAIT_US   = 70;
    static constexpr int PRESENCE_FINISH_US = 410;
    
    static constexpr int WRITE1_LOW_US      = 6;
    static constexpr int WRITE1_FINISH_US   = 64;
    static constexpr int WRITE0_LOW_US      = 60;
    static constexpr int WRITE0_FINISH_US   = 10;

    static constexpr int READ_INIT_US       = 6;
    static constexpr int READ_SAMPLE_US     = 15;
    static constexpr int READ_END_US        = 45;      
  }
  

using namespace PM;

OneWire* OneWire::Get(gpio_num_t pin, uint64_t rom_id) 
{
    // If the exact (pin + rom) instance exists, reuse it
    for (auto* inst : _instances) 
    {
        if (inst->_pin == pin && inst->_rom == rom_id)
            return inst;
    }

    // Else, create a new instance (ROM-aware)
    auto* newInst = new OneWire(pin, rom_id);
    _instances.push_back(newInst);
    return newInst;
}


OneWire* OneWire::Get(gpio_num_t pin) 
{
    for (auto* inst : _instances) {
        if (inst->_pin == pin) {
            if (inst->_rom == 0) return inst;  // Exact match
            ESP_LOGE("OneWire", "GPIO already in use by ROM-aware device");
            return nullptr; // TODO: handle

        }
    }

    auto* newInst = new OneWire(pin);
    _instances.push_back(newInst);
    return newInst;
}

void OneWire::init() 
{
    gpio_config_t cfg = {};
    cfg.intr_type    = GPIO_INTR_DISABLE;
    cfg.mode         = GPIO_MODE_INPUT_OUTPUT_OD;
    cfg.pin_bit_mask = 1ULL << static_cast<uint64_t>(_pin);
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);
}

void OneWire::writeBit(bool bit) 
{
    gpio_set_direction(_pin, GPIO_MODE_OUTPUT);

    if (bit) 
    { // Write '1' bit
    
        gpio_set_level(_pin, 0);                  // pull low
        esp_rom_delay_us(WRITE1_LOW_US);          // e.g. 6 µs
        gpio_set_level(_pin, 1);                  // release
        esp_rom_delay_us(WRITE1_FINISH_US);       // e.g. 64 µs
    } 
    else 
    { // Write '0' bit
        gpio_set_level(_pin, 0);                  // pull low
        esp_rom_delay_us(WRITE0_LOW_US);          // e.g. 60 µs
        gpio_set_level(_pin, 1);                  // release
        esp_rom_delay_us(WRITE0_FINISH_US);       // e.g. 10 µs
    }
}

uint8_t OneWire::readBit() 
{
    gpio_set_direction(_pin, GPIO_MODE_OUTPUT);

    // Write '0' bit for 1-15 us to start the read
    gpio_set_level(_pin, 0);
    esp_rom_delay_us(READ_INIT_US);

    // slave sending '1' -> do nothing, bus goes to pull-up
    // slave sending '0' -> bus goes low for 60 us
    gpio_set_level(_pin, 1);                      // release
    esp_rom_delay_us(READ_SAMPLE_US);             // e.g. 15 µs

    uint8_t bit = gpio_get_level(_pin);
    esp_rom_delay_us(READ_END_US); // wait 45 us
    return bit;
}

void OneWire::writeByte(uint8_t byte) 
{
    for (int i = 0; i < 8; i++) 
    {
        bool bit = byte & 0x01;    // Get the least significant bit (LSB)
        writeBit(bit);             // Write that bit
        byte >>= 1;                // Shift right to get the next bit
    }
}

uint8_t OneWire::readByte() 
{
    uint8_t value = 0;
    for (int i = 0; i < 8; i++) 
    {
        if (readBit()) 
            value |= (1 << i); // Set the i-th bit if readBit() returned 1
        
    }
    return value;
}

bool OneWire::reset() {
    // Drive reset pulse
    gpio_set_level(_pin, 0);                      // pull low
    esp_rom_delay_us(RESET_PULSE_US);             // e.g. 480 µs

    // Release and check for presence
    gpio_set_level(_pin, 1);                      // release
    esp_rom_delay_us(PRESENCE_WAIT_US);           // e.g. 70 µs
    bool presence = (gpio_get_level(_pin) == 0);  // bus low = device present
    esp_rom_delay_us(PRESENCE_FINISH_US);         // e.g. 410 µs
    return presence;
}
