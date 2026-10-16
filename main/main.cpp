#include "secrets.h"

//drivers
#include "dht_sensor.h"
#include "wifi.h"
#include "mqtt.h"
#include "ds18b20.h"
#include "time_sync.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_pm.h"

#include <cstdio>
#include <ctime>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// LAN-only setups can point this at the router or broker host in secrets.h
#ifndef SNTP_SERVER
#define SNTP_SERVER "pool.ntp.org"
#endif

constexpr float c_to_f(float c) { return c * 9.0f / 5.0f + 32.0f; }

extern "C" void app_main() {

    // general setup
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // wifi
    constexpr const int WIFI_CONNECT_TIMEOUT_MS = 15000;
    Wifi wifi(WIFI_SSID, WIFI_PASSWORD);
    auto rc = wifi.init();
    if (rc != Wifi::ReturnCode::OK) {
        ESP_LOGE("main", "wifi init failed: %d", static_cast<int>(rc));
        return;
    }

    rc = wifi.wait_for_connection(WIFI_CONNECT_TIMEOUT_MS);
    if (rc == Wifi::ReturnCode::OK) {
        ESP_LOGI("main", "wifi connected");
    } else {
        ESP_LOGW("main", "wifi not connected, rc=%d", static_cast<int>(rc));
    }

    // time: publishing is held until SNTP has set the clock (POOL-37). If the
    // first sync misses this window, SNTP keeps retrying in the background.
    constexpr const int TIME_SYNC_TIMEOUT_MS = 10000;
    if (time_sync_start(SNTP_SERVER) == ESP_OK && !time_sync_wait(TIME_SYNC_TIMEOUT_MS)) {
        ESP_LOGW("main", "time not synced yet, readings held until it is");
    }

    // power management: automatic light sleep when idle.
    // Keeps the association alive and RAM retained, unlike deep sleep.
    // Also limits self-heating, which matters since the DHT is measuring
    // the same enclosure this board sits in.
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));
    wifi.set_power_save(true);

    // mqtt
    constexpr const int MQTT_CONNECT_TIMEOUT_MS = 10000;
    char status_topic[96];
    char telemetry_topic[96];
    snprintf(status_topic, sizeof(status_topic), "pool/%s/status", MQTT_DEVICE_ID);
    snprintf(telemetry_topic, sizeof(telemetry_topic), "pool/%s/telemetry", MQTT_DEVICE_ID);

    MqttClient mqtt(MQTT_BROKER_URI, MQTT_DEVICE_ID);
    auto mrc = mqtt.init();
    if (mrc != MqttClient::ReturnCode::OK) {
        ESP_LOGE("main", "mqtt init failed: %d", static_cast<int>(mrc));
    }

    mrc = mqtt.wait_for_connection(MQTT_CONNECT_TIMEOUT_MS);
    if (mrc == MqttClient::ReturnCode::OK) {
        ESP_LOGI("main", "mqtt connected");
        mqtt.publish(status_topic, "online", 1, true);
    } else {
        ESP_LOGW("main", "mqtt not connected, rc=%d", static_cast<int>(mrc));
    }

    // dht sensor
    constexpr const uint32_t DHT_READ_INTERVAL_MS = 1800000;   // 30 minutes
    DhtSensor dht(GPIO_NUM_4, DhtSensor::DHT22);
    ESP_ERROR_CHECK(dht.init());

    // water temperature: one DS18B20 on its own 1-Wire bus (POOL-9)
    constexpr gpio_num_t ONEWIRE_PIN = GPIO_NUM_27;
    OneWire onewire(ONEWIRE_PIN);
    ESP_ERROR_CHECK(onewire.init());

    // Find the probe's ROM ID instead of hardcoding it, so a swapped probe
    // needs no reflash. If none is found, rom stays 0 and init() fails, which
    // leaves the sensor skipped in the loop.
    uint64_t rom = 0;
    if (onewire.search(&rom, 1) == 1) {
        ESP_LOGI("ds18b20", "probe on GPIO %d: ROM %016llX", ONEWIRE_PIN, rom);
    } else {
        ESP_LOGE("ds18b20", "no probe found on GPIO %d", ONEWIRE_PIN);
    }
    Ds18b20 water(onewire, rom);
    water.init();

    uint32_t seq = 0;
    char payload[128];

    // One flat message per reading (POOL-38); ts is when the reading was taken
    auto publish_reading = [&](const char* id, time_t ts, float v) {
        snprintf(payload, sizeof(payload),
                 "{\"schema\":1,\"seq\":%lu,\"id\":\"%s\",\"ts\":%lld,\"v\":%.2f}",
                 seq++, id, static_cast<long long>(ts), v);
        if (mqtt.publish(telemetry_topic, payload) != MqttClient::ReturnCode::OK) {
            ESP_LOGW("main", "publish %s failed", id);
        }
    };

    while (true) {
        // A reading with a bogus epoch looks valid downstream; a held one is
        // just a gap. Sensors are still sampled so their health stays current.
        bool time_ok = time_is_synced();
        if (!time_ok) {
            ESP_LOGW("main", "time not synced, holding readings");
        }

        auto drc = dht.sample();
        time_t ts = time(nullptr);

        if (drc == DhtSensor::ReturnCode::DHT_OK) {
            auto d = dht.read_data();
            ESP_LOGI("dht", "%.1f F  %.1f %%RH", c_to_f(d.temperature), d.humidity);

            if (time_ok) {
                publish_reading("ENC_T", ts, d.temperature);
                publish_reading("ENC_RH", ts, d.humidity);
            }
        } else {
            ESP_LOGW("dht", "sample failed, rc=%d", static_cast<int>(drc));
        }

        if (water.get_status() != Ds18b20::Status::UNINITIALIZED) {
            auto wrc = water.sample();
            ts = time(nullptr);

            if (wrc == Ds18b20::ReturnCode::OK) {
                auto w = water.read_data();
                ESP_LOGI("ds18b20", "water %.2f C (%.1f F)", w.temperature, c_to_f(w.temperature));

                if (time_ok) {
                    publish_reading("WATER_T", ts, w.temperature);
                }
            } else {
                ESP_LOGW("ds18b20", "sample failed, rc=%d", static_cast<int>(wrc));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DHT_READ_INTERVAL_MS));
    }
}
