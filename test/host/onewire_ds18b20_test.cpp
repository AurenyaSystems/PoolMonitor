// Host test: OneWire search/CRC and the DS18B20 driver over a simulated bus.
// Host test: simulated 1-Wire bus of DS18B20s, decoded from GPIO pulse widths.
#include "onewire.h"
#include "ds18b20.h"
#include <vector>
#include <set>
#include <cstdio>
#include <cstring>
#include <cmath>

int g_critical_depth = 0;
int64_t g_fake_time_us = 0;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

struct Dev { uint64_t rom; uint8_t sp[9]; bool active; bool selected; };
static std::vector<Dev> devs;

// --- bus state machine ---
enum class Phase { IDLE, ROM_CMD, SEARCH, MATCH, FUNC_CMD, SEND };
static Phase phase = Phase::IDLE;
static int bitpos = 0;          // bits collected / sent in current phase
static uint64_t acc = 0;        // accumulator for written bits
static int search_bit = 0, search_step = 0;
static bool line_low = false, slot_pending = false, presence_pending = false;
static uint32_t low_us = 0;
static int max_low_in_critical_violation = 0;

static void start_phase(Phase p) { phase = p; bitpos = 0; acc = 0; }

static Dev* selected_dev() { for (auto& d : devs) if (d.selected) return &d; return nullptr; }

static void device_write(bool bit) {
    switch (phase) {
    case Phase::ROM_CMD:
        acc |= (uint64_t)bit << bitpos++;
        if (bitpos == 8) {
            uint8_t cmd = acc;
            if (cmd == 0xF0) { start_phase(Phase::SEARCH); search_bit = 0; search_step = 0; for (auto& d : devs) d.active = true; }
            else if (cmd == 0x55) start_phase(Phase::MATCH);
            else { printf("unexpected ROM cmd %02X\n", cmd); failures++; start_phase(Phase::IDLE); }
        }
        break;
    case Phase::SEARCH:
        if (search_step != 2) { printf("search: write at wrong step\n"); failures++; }
        for (auto& d : devs) if (d.active && (((d.rom >> search_bit) & 1) != bit)) d.active = false;
        search_bit++; search_step = 0;
        break;
    case Phase::MATCH:
        acc |= (uint64_t)bit << bitpos++;
        if (bitpos == 64) {
            for (auto& d : devs) d.selected = (d.rom == acc);
            start_phase(Phase::FUNC_CMD);
        }
        break;
    case Phase::FUNC_CMD:
        acc |= (uint64_t)bit << bitpos++;
        if (bitpos == 8) {
            uint8_t cmd = acc;
            if (cmd == 0x44) start_phase(Phase::IDLE);            // convert: value already in sp
            else if (cmd == 0xBE) start_phase(Phase::SEND);
            else { printf("unexpected func cmd %02X\n", cmd); failures++; }
        }
        break;
    default: printf("write in phase %d\n", (int)phase); failures++;
    }
}

static bool device_read() {
    if (phase == Phase::SEARCH) {
        bool any0 = false, any1 = false;
        for (auto& d : devs) if (d.active) { if ((d.rom >> search_bit) & 1) any1 = true; else any0 = true; }
        bool r;
        if (search_step == 0) r = !any0;          // wired-AND of id bits: low if any device has 0
        else if (search_step == 1) r = !any1;     // complement bits
        else { printf("search: read at step 2\n"); failures++; r = 1; }
        search_step++;
        return r;
    }
    if (phase == Phase::SEND) {
        Dev* d = selected_dev();
        if (!d || bitpos >= 72) return 1;
        bool r = (d->sp[bitpos / 8] >> (bitpos % 8)) & 1;
        bitpos++;
        return r;
    }
    return 1;
}

static void finalize_write() {
    if (!slot_pending) return;
    slot_pending = false;
    device_write(low_us < 15);
}

int gpio_config(const gpio_config_t*) { return 0; }
int gpio_set_level(gpio_num_t, uint32_t level) {
    if (level == 0) {
        finalize_write();
        if (g_critical_depth == 0) max_low_in_critical_violation++;
        line_low = true; low_us = 0;
    } else if (line_low) {
        line_low = false;
        if (low_us >= 480) {                       // reset pulse
            slot_pending = false;
            presence_pending = true;
            for (auto& d : devs) d.selected = false;
            start_phase(Phase::ROM_CMD);
        } else {
            slot_pending = true;
        }
    }
    return 0;
}
int gpio_get_level(gpio_num_t) {
    if (presence_pending) { presence_pending = false; return devs.empty() ? 1 : 0; }
    if (slot_pending) { slot_pending = false; return device_read(); }
    return 1;
}
void esp_rom_delay_us(uint32_t us) { if (line_low) low_us += us; }

// --- helpers ---
static uint64_t make_rom(uint8_t family, uint64_t serial48) {
    uint8_t b[8]; b[0] = family;
    for (int i = 0; i < 6; i++) b[1 + i] = (serial48 >> (8 * i)) & 0xFF;
    b[7] = OneWire::crc8(b, 7);
    uint64_t r = 0; for (int i = 0; i < 8; i++) r |= (uint64_t)b[i] << (8 * i);
    return r;
}
static void set_temp(Dev& d, int16_t raw) {
    uint8_t sp[9] = { (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8), 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0 };
    sp[8] = OneWire::crc8(sp, 8);
    memcpy(d.sp, sp, 9);
}
static Dev dev(uint64_t rom, int16_t raw) { Dev d{rom, {}, false, false}; set_temp(d, raw); return d; }

static void test_search(std::vector<uint64_t> roms) {
    devs.clear(); for (auto r : roms) devs.push_back(dev(r, 0x0191));
    OneWire bus(GPIO_NUM_27); bus.init();
    uint64_t found[8] = {};
    int n = bus.search(found, 8);
    printf("search: want %zu got %d\n", roms.size(), n); for (int i = 0; i < n; i++) printf("   %016lX\n", found[i]); CHECK(n == (int)roms.size());
    std::set<uint64_t> want(roms.begin(), roms.end()), got(found, found + n);
    CHECK(want == got);
}

int main() {
    // CRC-8 known vector from Maxim AN27: ROM 02 1C B8 01 00 00 00 A2
    { uint8_t v[8] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xA2}; CHECK(OneWire::crc8(v, 7) == 0xA2); CHECK(OneWire::crc8(v, 8) == 0); }

    // Search: empty, one, two, several (incl. ROMs differing in a single bit)
    test_search({});
    test_search({ make_rom(0x28, 0x0000000001B8ULL) });
    test_search({ make_rom(0x28, 0x112233445566ULL), make_rom(0x28, 0xA1B2C3D4E5F6ULL) });
    test_search({ make_rom(0x28, 1), make_rom(0x28, 2), make_rom(0x28, 3), make_rom(0x10, 3), make_rom(0x28, 0xFFFFFFFFFFFFULL) });

    // max_roms limit
    { devs.clear(); for (int i = 1; i <= 3; i++) devs.push_back(dev(make_rom(0x28, i), 0)); OneWire bus(GPIO_NUM_27); uint64_t f[2]; CHECK(bus.search(f, 2) == 2); }

    // DS18B20: two probes on one bus, each reads its own value via Match ROM
    uint64_t ra = make_rom(0x28, 0x0102030405ULL), rb = make_rom(0x28, 0x0A0B0C0D0EULL);
    devs.clear(); devs.push_back(dev(ra, 0x0191)); devs.push_back(dev(rb, (int16_t)0xFF5E));
    OneWire bus(GPIO_NUM_27); bus.init();
    Ds18b20 a(bus, ra), b(bus, rb);
    CHECK(a.init() == Ds18b20::ReturnCode::OK);
    CHECK(b.init() == Ds18b20::ReturnCode::OK);
    CHECK(a.sample() == Ds18b20::ReturnCode::OK); CHECK(a.read_data().temperature == 25.0625f);
    CHECK(b.sample() == Ds18b20::ReturnCode::OK); CHECK(b.read_data().temperature == -10.125f);
    CHECK(a.get_status() == Ds18b20::Status::READY);

    // Datasheet table extremes
    set_temp(devs[0], (int16_t)0x07D0); CHECK(a.sample() == Ds18b20::ReturnCode::OK); CHECK(a.read_data().temperature == 125.0f);
    set_temp(devs[0], (int16_t)0xFC90); CHECK(a.sample() == Ds18b20::ReturnCode::OK); CHECK(a.read_data().temperature == -55.0f);

    // 85.0 power-on value rejected, last good reading kept
    set_temp(devs[0], 0x0550); CHECK(a.sample() == Ds18b20::ReturnCode::IMPLAUSIBLE_READING);
    CHECK(a.read_data().temperature == -55.0f); CHECK(a.get_status() == Ds18b20::Status::DEGRADED);

    // Corrupt CRC
    set_temp(devs[0], 0x0191); devs[0].sp[8] ^= 0x01; CHECK(a.sample() == Ds18b20::ReturnCode::CRC_MISMATCH);

    // All-zero scratchpad passes CRC but is rejected
    memset(devs[0].sp, 0, 9); CHECK(OneWire::crc8(devs[0].sp, 9) == 0); CHECK(a.sample() == Ds18b20::ReturnCode::IMPLAUSIBLE_READING);

    // Recovers after a good read
    set_temp(devs[0], 0x0191); CHECK(a.sample() == Ds18b20::ReturnCode::OK); CHECK(a.get_status() == Ds18b20::Status::READY);

    // FAILED after 5 consecutive failures
    set_temp(devs[0], 0x0550);
    for (int i = 0; i < 4; i++) a.sample();
    CHECK(a.get_status() == Ds18b20::Status::DEGRADED);
    a.sample(); CHECK(a.get_status() == Ds18b20::Status::FAILED);

    // Wrong family / absent ROM / empty bus
    { Ds18b20 x(bus, make_rom(0x10, 5)); CHECK(x.init() == Ds18b20::ReturnCode::NOT_A_DS18B20); CHECK(x.sample() == Ds18b20::ReturnCode::NOT_INITIALIZED); }
    { Ds18b20 x(bus, 0); CHECK(x.init() == Ds18b20::ReturnCode::NOT_A_DS18B20); }
    { auto saved = devs; devs.clear(); Ds18b20 x(bus, ra); CHECK(x.init() == Ds18b20::ReturnCode::NO_PRESENCE); devs = saved; }
    // Probe unplugged after init: select fails
    { auto saved = devs; Ds18b20 x(bus, ra); CHECK(x.init() == Ds18b20::ReturnCode::OK); devs.clear(); CHECK(x.sample() == Ds18b20::ReturnCode::NO_PRESENCE); devs = saved; }

    CHECK(g_critical_depth == 0);
    CHECK(max_low_in_critical_violation == 0);   // every bus-low edge happened inside a critical section
    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures != 0;
}
