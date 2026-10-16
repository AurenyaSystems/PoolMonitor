// Host test: EzoSensor against a fake I2C device (status bytes, parsing, commands).
#include "ezo_sensor.h"
#include <string>
#include <vector>
#include <deque>
#include <cstring>
#include <cstdio>
int g_critical_depth = 0;
int64_t g_fake_time_us = 0;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static std::vector<std::string> sent;            // commands the driver wrote
static std::deque<std::vector<uint8_t>> replies; // raw buffers the fake returns, in order
static bool nack = false;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t*, i2c_master_dev_handle_t* h) { *h = (i2c_master_dev_handle_t)1; return ESP_OK; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t* b, size_t n, int) { if (nack) return ESP_FAIL; sent.emplace_back((const char*)b, n); return ESP_OK; }
esp_err_t i2c_master_receive(i2c_master_dev_handle_t, uint8_t* b, size_t n, int) {
    if (nack) return ESP_FAIL;
    memset(b, 0, n);
    if (replies.empty()) { b[0] = 255; return ESP_OK; }
    auto r = replies.front(); replies.pop_front();
    memcpy(b, r.data(), std::min(n, r.size()));
    return ESP_OK;
}
static void reply(uint8_t code, const char* text = "") { std::vector<uint8_t> v{code}; for (const char* p = text; *p; p++) v.push_back(*p); v.push_back(0); replies.push_back(v); }
static void reply_raw(std::vector<uint8_t> v) { replies.push_back(v); }

int main() {
    using RC = EzoSensor::ReturnCode;
    EzoSensor ph((i2c_master_bus_handle_t)1, 0x63, 0.0f, 14.0f);

    CHECK(ph.sample() == RC::NOT_INITIALIZED);

    reply(1, "?I,pH,2.16");
    CHECK(ph.init() == RC::OK); CHECK(sent.back() == "i"); CHECK(ph.get_status() == EzoSensor::Status::READY);

    reply(1, "7.42");
    CHECK(ph.sample() == RC::OK); CHECK(sent.back() == "R"); CHECK(ph.read_data().value == 7.42f);

    // still processing twice, then success
    reply(254); reply(254); reply(1, "7.50");
    CHECK(ph.sample() == RC::OK); CHECK(ph.read_data().value == 7.50f);

    // stuck processing beyond the retry window
    for (int i = 0; i < 10; i++) reply(254);
    CHECK(ph.sample() == RC::NOT_READY); replies.clear();
    CHECK(ph.get_status() == EzoSensor::Status::DEGRADED);
    CHECK(ph.read_data().value == 7.50f);   // last good kept

    reply(2);               CHECK(ph.sample() == RC::SYNTAX_ERROR);
    reply(255);             CHECK(ph.sample() == RC::NO_DATA);
    reply(1, "abc");        CHECK(ph.sample() == RC::PARSE_ERROR);
    reply(1, "7.4x");       CHECK(ph.sample() == RC::PARSE_ERROR);
    reply(1, "");           CHECK(ph.sample() == RC::PARSE_ERROR);
    CHECK(ph.get_status() == EzoSensor::Status::FAILED);
    reply(1, "15.2");       CHECK(ph.sample() == RC::IMPLAUSIBLE_READING);
    reply(1, "6.98");       CHECK(ph.sample() == RC::OK); CHECK(ph.get_status() == EzoSensor::Status::READY);

    // reply with no NUL terminator filling the whole buffer must not overrun
    { std::vector<uint8_t> v(32, '7'); v[0] = 1; v[2] = '.'; reply_raw(v); CHECK(ph.sample() == RC::IMPLAUSIBLE_READING || ph.read_data().value > 0); }

    // temperature compensation command format
    reply(1); CHECK(ph.set_temperature(26.437f) == RC::OK); CHECK(sent.back() == "T,26.44");
    reply(1); CHECK(ph.set_temperature(-1.5f) == RC::OK); CHECK(sent.back() == "T,-1.50");

    // calibration
    reply(1); CHECK(ph.calibrate("mid,7.00") == RC::OK); CHECK(sent.back() == "Cal,mid,7.00");
    reply(2); CHECK(ph.calibrate("bogus") == RC::SYNTAX_ERROR);
    int pts = -1; reply(1, "?CAL,3"); CHECK(ph.calibration_points(pts) == RC::OK); CHECK(pts == 3); CHECK(sent.back() == "Cal,?");
    reply(1, "garbage"); CHECK(ph.calibration_points(pts) == RC::PARSE_ERROR);

    // ORP: negative mV and its range
    EzoSensor orp((i2c_master_bus_handle_t)1, 0x62, -1019.9f, 1019.9f);
    reply(1, "?I,OR,2.10"); CHECK(orp.init() == RC::OK);
    reply(1, "-210.4"); CHECK(orp.sample() == RC::OK); CHECK(orp.read_data().value == -210.4f);
    reply(1, "1100.0"); CHECK(orp.sample() == RC::IMPLAUSIBLE_READING);

    // init failures
    { EzoSensor x((i2c_master_bus_handle_t)1, 0x10, 0, 14); nack = true; CHECK(x.init() == RC::I2C_ERROR); nack = false; CHECK(x.get_status() == EzoSensor::Status::UNINITIALIZED); }
    { EzoSensor x((i2c_master_bus_handle_t)1, 0x10, 0, 14); reply(1, "hello"); CHECK(x.init() == RC::PARSE_ERROR); }
    { EzoSensor x((i2c_master_bus_handle_t)1, 0x10, 0, 14); reply(255); CHECK(x.init() == RC::NO_DATA); }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures != 0;
}
