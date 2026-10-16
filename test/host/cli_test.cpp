// Host test for the CLI dispatcher, driving the real driver classes over the
// simulated 1-Wire bus and a fake I2C device. Wifi/MQTT are left null.
#include "cli_class.h"
#include "time_sync.h"
#include <deque>
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>

int g_critical_depth = 0;
int g_lock_depth = 0;
int64_t g_fake_time_us = 3000000;   // past the DHT's 2 s power-on hold-off
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
extern std::vector<std::pair<std::string, std::string>> published;
extern bool g_publish_ok;
#define CONTAINS(s, sub) (strstr((s), (sub)) != nullptr)

// ---- time_sync stub
static bool g_synced = true;
esp_err_t time_sync_start(const char*) { return ESP_OK; }
bool time_sync_wait(uint32_t) { return g_synced; }
bool time_is_synced() { return g_synced; }

// ---- FreeRTOS queue fake
struct FakeQueue { size_t depth, item; std::deque<std::vector<uint8_t>> q; };
QueueHandle_t xQueueCreate(size_t depth, size_t item) { return new FakeQueue{depth, item, {}}; }
int xQueueSend(QueueHandle_t h, const void* p, uint32_t) { auto* q = (FakeQueue*)h; if (q->q.size() >= q->depth) return pdFALSE; q->q.emplace_back((const uint8_t*)p, (const uint8_t*)p + q->item); return pdTRUE; }
int xQueueReceive(QueueHandle_t h, void* p, uint32_t) { auto* q = (FakeQueue*)h; if (q->q.empty()) return pdFALSE; memcpy(p, q->q.front().data(), q->item); q->q.pop_front(); return pdTRUE; }
size_t uxQueueMessagesWaiting(QueueHandle_t h) { return ((FakeQueue*)h)->q.size(); }

// ---- NVS fake
static std::map<std::string, uint32_t> nvs;
esp_err_t nvs_open(const char*, int, nvs_handle_t* h) { *h = 1; return ESP_OK; }
esp_err_t nvs_get_u32(nvs_handle_t, const char* k, uint32_t* v) { auto it = nvs.find(k); if (it == nvs.end()) return ESP_ERR_NVS_NOT_FOUND; *v = it->second; return ESP_OK; }
esp_err_t nvs_set_u32(nvs_handle_t, const char* k, uint32_t v) { nvs[k] = v; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
void nvs_close(nvs_handle_t) {}

// ---- I2C fake: one reply queue per device address
static std::map<uintptr_t, std::deque<std::vector<uint8_t>>> i2c_replies;
static std::map<uintptr_t, std::vector<std::string>> i2c_sent;
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t* c, i2c_master_dev_handle_t* h) { *h = (i2c_master_dev_handle_t)(uintptr_t)c->device_address; return ESP_OK; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t h, const uint8_t* b, size_t n, int) { i2c_sent[(uintptr_t)h].emplace_back((const char*)b, n); return ESP_OK; }
esp_err_t i2c_master_receive(i2c_master_dev_handle_t h, uint8_t* b, size_t n, int) {
    memset(b, 0, n);
    auto& q = i2c_replies[(uintptr_t)h];
    if (q.empty()) { b[0] = 255; return ESP_OK; }
    memcpy(b, q.front().data(), std::min(n, q.front().size())); q.pop_front(); return ESP_OK;
}
static void reply(uint8_t addr, uint8_t code, const char* text = "") { std::vector<uint8_t> v{code}; for (const char* p = text; *p; p++) v.push_back(*p); v.push_back(0); i2c_replies[addr].push_back(v); }
static const std::string& last_sent(uint8_t addr) { return i2c_sent[addr].back(); }

// ---- 1-Wire bus simulator (pin 27 only), one DS18B20
struct Dev { uint64_t rom; uint8_t sp[9]; bool active; bool selected; };
static std::vector<Dev> devs;
enum class Phase { IDLE, ROM_CMD, SEARCH, MATCH, FUNC_CMD, SEND };
static Phase phase = Phase::IDLE; static int bitpos = 0; static uint64_t acc = 0;
static int search_bit = 0, search_step = 0;
static bool line_low = false, slot_pending = false, presence_pending = false; static uint32_t low_us = 0;
static void start_phase(Phase p) { phase = p; bitpos = 0; acc = 0; }
static Dev* selected_dev() { for (auto& d : devs) if (d.selected) return &d; return nullptr; }
static void device_write(bool bit) {
    switch (phase) {
    case Phase::ROM_CMD: acc |= (uint64_t)bit << bitpos++; if (bitpos == 8) { uint8_t c = acc; if (c == 0xF0) { start_phase(Phase::SEARCH); search_bit = 0; search_step = 0; for (auto& d : devs) d.active = true; } else if (c == 0x55) start_phase(Phase::MATCH); else start_phase(Phase::IDLE); } break;
    case Phase::SEARCH: for (auto& d : devs) if (d.active && (((d.rom >> search_bit) & 1) != bit)) d.active = false; search_bit++; search_step = 0; break;
    case Phase::MATCH: acc |= (uint64_t)bit << bitpos++; if (bitpos == 64) { for (auto& d : devs) d.selected = (d.rom == acc); start_phase(Phase::FUNC_CMD); } break;
    case Phase::FUNC_CMD: acc |= (uint64_t)bit << bitpos++; if (bitpos == 8) { uint8_t c = acc; if (c == 0x44) start_phase(Phase::IDLE); else if (c == 0xBE) start_phase(Phase::SEND); } break;
    default: break;
    }
}
static bool device_read() {
    if (phase == Phase::SEARCH) { bool any0 = false, any1 = false; for (auto& d : devs) if (d.active) { if ((d.rom >> search_bit) & 1) any1 = true; else any0 = true; } bool r = (search_step == 0) ? !any0 : !any1; search_step++; return r; }
    if (phase == Phase::SEND) { Dev* d = selected_dev(); if (!d || bitpos >= 72) return 1; bool r = (d->sp[bitpos / 8] >> (bitpos % 8)) & 1; bitpos++; return r; }
    return 1;
}
static void finalize_write() { if (!slot_pending) return; slot_pending = false; device_write(low_us < 15); }
int gpio_config(const gpio_config_t*) { return 0; }
int gpio_set_level(gpio_num_t pin, uint32_t level) {
    if (pin != 27) return 0;
    if (level == 0) { finalize_write(); line_low = true; low_us = 0; }
    else if (line_low) { line_low = false; if (low_us >= 480) { slot_pending = false; presence_pending = true; for (auto& d : devs) d.selected = false; start_phase(Phase::ROM_CMD); } else slot_pending = true; }
    return 0;
}
int gpio_get_level(gpio_num_t pin) {
    if (pin != 27) return 1;   // DHT line idle high: its reads time out
    if (presence_pending) { presence_pending = false; return devs.empty() ? 1 : 0; }
    if (slot_pending) { slot_pending = false; return device_read(); }
    return 1;
}
void esp_rom_delay_us(uint32_t us) { if (line_low) low_us += us; }
static uint64_t make_rom(uint8_t family, uint64_t serial) { uint8_t b[8]; b[0] = family; for (int i = 0; i < 6; i++) b[1 + i] = (serial >> (8 * i)) & 0xFF; b[7] = OneWire::crc8(b, 7); uint64_t r = 0; for (int i = 0; i < 8; i++) r |= (uint64_t)b[i] << (8 * i); return r; }
static void set_temp(Dev& d, int16_t raw) { uint8_t sp[9] = { (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8), 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0 }; sp[8] = OneWire::crc8(sp, 8); memcpy(d.sp, sp, 9); }

// ---- helpers
static char out[Cli::MAX_REPLY];
static int run(Cli& cli, const char* line) { int rc = cli.execute(line, out, sizeof(out)); CHECK(g_lock_depth == 0); return rc; }

int main() {
    // hardware
    uint64_t rom = make_rom(0x28, 0x0102030405ULL);
    devs.push_back({rom, {}, false, false}); set_temp(devs[0], 0x0191);   // 25.0625 C
    OneWire bus(GPIO_NUM_27); bus.init();
    uint64_t found = 0; CHECK(bus.search(&found, 1) == 1 && found == rom);
    Ds18b20 water(bus, found); CHECK(water.init() == Ds18b20::ReturnCode::OK);
    DhtSensor dht(GPIO_NUM_4, DhtSensor::DHT22); dht.init();
    EzoSensor ph((i2c_master_bus_handle_t)1, 0x63, 0.0f, 14.0f);  reply(0x63, 1, "?I,pH,2.16"); CHECK(ph.init() == EzoSensor::ReturnCode::OK);
    EzoSensor orp((i2c_master_bus_handle_t)1, 0x62, -1019.9f, 1019.9f); reply(0x62, 1, "?I,OR,2.10"); CHECK(orp.init() == EzoSensor::ReturnCode::OK);
    ReadingQueue queue; CHECK(queue.init(4));
    SeqCounter seq; nvs["next_seq"] = 42; CHECK(seq.init() == SeqCounter::ReturnCode::OK);

    MqttClient mqtt("mqtt://x", "dev");
    Cli cli({ &dht, &water, &ph, &orp, nullptr, nullptr, &queue, &seq, "pool/dev/sysinfo" });

    // dispatcher basics
    CHECK(run(cli, "help") == 0); CHECK(CONTAINS(out, "ph read")); CHECK(CONTAINS(out, "queue status"));
    CHECK(run(cli, "bogus") == 1); CHECK(CONTAINS(out, "unknown command 'bogus'"));
    CHECK(run(cli, "") == 0); CHECK(out[0] == '\0');
    CHECK(run(cli, "   \r\n") == 0); CHECK(out[0] == '\0');
    CHECK(run(cli, nullptr) == 1);
    CHECK(run(cli, "  water   read  \n") == 0); CHECK(!strcmp(out, "water 25.06C"));   // whitespace tolerant

    // status: one line per subsystem, null wifi/mqtt omitted
    CHECK(run(cli, "status") == 0);
    CHECK(!CONTAINS(out, "wifi")); CHECK(!CONTAINS(out, "mqtt"));
    CHECK(CONTAINS(out, "time synced=yes")); CHECK(CONTAINS(out, "seq next=42"));
    CHECK(CONTAINS(out, "queue 0 waiting, 0 dropped")); CHECK(CONTAINS(out, "dht READY"));
    CHECK(CONTAINS(out, "water READY 25.06C")); CHECK(CONTAINS(out, "ph READY")); CHECK(CONTAINS(out, "orp READY"));
    CHECK(out[strlen(out) - 1] != '\n');

    // water
    CHECK(run(cli, "water rom") == 0); CHECK(CONTAINS(out, "rom=")); CHECK(CONTAINS(out, "28"));
    set_temp(devs[0], (int16_t)0xFF50); CHECK(run(cli, "water read") == 0); CHECK(!strcmp(out, "water -11.00C"));
    CHECK(run(cli, "water status") == 0); CHECK(CONTAINS(out, "water READY last=-11.00C age="));
    set_temp(devs[0], 0x0550); CHECK(run(cli, "water read") == 1); CHECK(CONTAINS(out, "error: water sample failed rc=5"));
    CHECK(run(cli, "water") == 1); CHECK(CONTAINS(out, "usage: water"));
    CHECK(run(cli, "water fly") == 1); CHECK(CONTAINS(out, "usage: water"));

    // dht: line idle high, so a read times out; the error path must report it
    CHECK(run(cli, "dht read") == 1); CHECK(CONTAINS(out, "error: dht sample failed rc=1"));
    CHECK(run(cli, "dht status") == 0); CHECK(CONTAINS(out, "dht DEGRADED last=nanC"));

    // ph
    reply(0x63, 1, "7.42"); CHECK(run(cli, "ph read") == 0); CHECK(!strcmp(out, "ph 7.42")); CHECK(last_sent(0x63) == "R");
    reply(0x63, 1); CHECK(run(cli, "ph temp 26.437") == 0); CHECK(!strcmp(out, "ph temp=26.44")); CHECK(last_sent(0x63) == "T,26.44");
    CHECK(run(cli, "ph temp") == 1); CHECK(CONTAINS(out, "usage: temp"));
    CHECK(run(cli, "ph temp warm") == 1);
    reply(0x63, 1); reply(0x63, 1, "?CAL,1"); CHECK(run(cli, "ph cal mid 7.00") == 0); CHECK(!strcmp(out, "ph calibrated mid,7.00 points=1"));
    CHECK(published.empty());   // mqtt target is null in this Cli: no record, no crash
    CHECK(i2c_sent[0x63][i2c_sent[0x63].size() - 2] == "Cal,mid,7.00"); CHECK(last_sent(0x63) == "Cal,?");
    reply(0x63, 1); reply(0x63, 1, "?CAL,3"); CHECK(run(cli, "ph cal high 10.00") == 0); CHECK(CONTAINS(out, "points=3"));
    reply(0x63, 1); reply(0x63, 1, "?CAL,0"); CHECK(run(cli, "ph cal clear") == 0); CHECK(!strcmp(out, "ph calibrated clear points=0")); CHECK(i2c_sent[0x63][i2c_sent[0x63].size() - 2] == "Cal,clear");
    reply(0x63, 1, "?CAL,2"); CHECK(run(cli, "ph cal ?") == 0); CHECK(!strcmp(out, "ph cal points=2"));
    reply(0x63, 2); CHECK(run(cli, "ph cal bogus") == 1); CHECK(CONTAINS(out, "error: ph cal bogus failed rc=3"));
    CHECK(run(cli, "ph cal") == 1); CHECK(CONTAINS(out, "usage: cal"));
    CHECK(run(cli, "ph status") == 0); CHECK(CONTAINS(out, "ph READY addr=0x63 last=7.42"));

    // orp
    reply(0x62, 1, "-210.4"); CHECK(run(cli, "orp read") == 0); CHECK(!strcmp(out, "orp -210.40"));
    reply(0x62, 1); reply(0x62, 1, "?CAL,1"); CHECK(run(cli, "orp cal 225") == 0); CHECK(!strcmp(out, "orp calibrated 225 points=1")); CHECK(i2c_sent[0x62][i2c_sent[0x62].size() - 2] == "Cal,225");
    reply(0x62, 1, "1500"); CHECK(run(cli, "orp read") == 1); CHECK(CONTAINS(out, "rc=7"));
    CHECK(run(cli, "orp status") == 0); CHECK(CONTAINS(out, "orp DEGRADED"));

    // queue + seq
    CHECK(run(cli, "queue status") == 0); CHECK(!strcmp(out, "queue 0 waiting, 0 dropped"));
    g_synced = false; CHECK(run(cli, "queue push PH 7.1") == 1); CHECK(CONTAINS(out, "time not synced")); g_synced = true;
    CHECK(run(cli, "queue push PH 7.1") == 0); CHECK(CONTAINS(out, "queued PH=7.10"));
    CHECK(run(cli, "queue push") == 1); CHECK(run(cli, "queue push PH x") == 1);
    CHECK(run(cli, "queue status") == 0); CHECK(!strcmp(out, "queue 1 waiting, 0 dropped"));
    Reading r; CHECK(queue.pop(r)); CHECK(!strcmp(r.id, "PH")); CHECK(r.v == 7.1f);
    for (int i = 0; i < 6; i++) run(cli, "queue push WATER_T 1");   // depth 4: two drops
    CHECK(run(cli, "queue status") == 0); CHECK(!strcmp(out, "queue 4 waiting, 2 dropped"));
    CHECK(run(cli, "seq") == 0); CHECK(!strcmp(out, "seq next=42")); seq.next(); CHECK(run(cli, "seq") == 0); CHECK(!strcmp(out, "seq next=43"));
    CHECK(nvs["next_seq"] == 43);

    // calibration records go out as retained sysinfo leaves when mqtt is present
    {
        Cli with_mqtt({ &dht, &water, &ph, &orp, nullptr, &mqtt, &queue, &seq, "pool/dev/sysinfo" });
        published.clear();
        reply(0x63, 1); reply(0x63, 1, "?CAL,2"); CHECK(with_mqtt.execute("ph cal low 4.00", out, sizeof(out)) == 0);
        CHECK(published.size() == 1); CHECK(published[0].first == "pool/dev/sysinfo/calibration/PH");
        CHECK(published[0].second.rfind("{\"ts\":", 0) == 0); CHECK(CONTAINS(published[0].second.c_str(), ",\"cmd\":\"low,4.00\",\"points\":2}"));
        g_synced = false; reply(0x62, 1); reply(0x62, 1, "?CAL,1"); CHECK(with_mqtt.execute("orp cal 225", out, sizeof(out)) == 0); g_synced = true;
        CHECK(published.size() == 2); CHECK(published[1].first == "pool/dev/sysinfo/calibration/ORP"); CHECK(published[1].second == "{\"cmd\":\"225\",\"points\":1}");
        reply(0x62, 2); CHECK(with_mqtt.execute("orp cal nope", out, sizeof(out)) == 1); CHECK(published.size() == 2);   // failed cal: no record
        g_publish_ok = false; reply(0x62, 1); reply(0x62, 1, "?CAL,1"); CHECK(with_mqtt.execute("orp cal 225", out, sizeof(out)) == 0); g_publish_ok = true;   // publish failure doesn't fail the command
        CHECK(with_mqtt.execute("mqtt status", out, sizeof(out)) == 0); CHECK(CONTAINS(out, "mqtt READY connected=yes"));
        CHECK(with_mqtt.execute("status", out, sizeof(out)) == 0); CHECK(CONTAINS(out, "mqtt READY"));
    }
    CHECK(publish_sysinfo(nullptr, "p", "l", "x") == false);
    CHECK(publish_sysinfo(&mqtt, nullptr, "l", "x") == false);
    published.clear(); CHECK(publish_sysinfo(&mqtt, "pool/dev/sysinfo", "version", "v1") == true); CHECK(published[0].first == "pool/dev/sysinfo/version");

    // absent targets and misc
    CHECK(run(cli, "wifi status") == 1); CHECK(!strcmp(out, "error: wifi not present"));
    CHECK(run(cli, "mqtt status") == 1); CHECK(!strcmp(out, "error: mqtt not present"));
    CHECK(run(cli, "time") == 0); CHECK(CONTAINS(out, "synced=yes")); CHECK(CONTAINS(out, "utc="));
    CHECK(run(cli, "reboot") == 0); CHECK(CONTAINS(out, "rebooting"));
    { Cli bare({ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }); CHECK(bare.execute("status", out, sizeof(out)) == 0); CHECK(CONTAINS(out, "time synced")); CHECK(bare.execute("ph read", out, sizeof(out)) == 1); }

    // robustness: over-long line, too many args, tiny output buffer
    std::string longline = "ph cal " + std::string(300, 'x'); CHECK(run(cli, longline.c_str()) == 1);
    CHECK(run(cli, "a b c d e f g h i j k l") == 1); CHECK(CONTAINS(out, "unknown command 'a'"));
    char tiny[8]; CHECK(cli.execute("status", tiny, sizeof(tiny)) == 0); CHECK(strlen(tiny) == 7);
    CHECK(cli.execute("help", tiny, sizeof(tiny)) == 0); CHECK(strlen(tiny) == 7);
    CHECK(cli.execute("help", tiny, 0) == 1);

    cli.start_console();   // stubs: just checks the front end compiles and links

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures != 0;
}

// ---- Wifi is never instantiated; MqttClient is a recorder
bool Wifi::has_ip() const { return false; }
Wifi::ReturnCode Wifi::set_power_save(bool) { return ReturnCode::OK; }
MqttClient::MqttClient(const char*, const char*): _client(nullptr), _events(nullptr), _status(Status::READY), _on_message(nullptr), _on_message_arg(nullptr) {}
bool MqttClient::is_connected() const { return true; }
std::vector<std::pair<std::string, std::string>> published;   // topic, payload
bool g_publish_ok = true;
MqttClient::ReturnCode MqttClient::publish(const char* t, const char* p, int, bool retain) { if (!g_publish_ok) return ReturnCode::NOT_CONNECTED; CHECK(retain); published.emplace_back(t, p); return ReturnCode::OK; }
