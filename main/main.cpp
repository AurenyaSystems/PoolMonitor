#include "secrets.h"

//drivers
#include "dht_sensor.h"
#include "wifi.h"
#include "mqtt.h"
#include "ds18b20.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_pm.h"

#include <cstdio>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

    // water temperature: DS18B20 probes sharing one 1-Wire bus (POOL-9)
    constexpr gpio_num_t ONEWIRE_PIN = GPIO_NUM_27;
    constexpr int MAX_WATER_PROBES = 2;
    OneWire onewire(ONEWIRE_PIN);
    ESP_ERROR_CHECK(onewire.init());

    uint64_t roms[MAX_WATER_PROBES] = {};
    int probe_count = onewire.search(roms, MAX_WATER_PROBES);
    ESP_LOGI("ds18b20", "found %d probe(s) on GPIO %d", probe_count, ONEWIRE_PIN);
    for (int i = 0; i < probe_count; i++) {
        ESP_LOGI("ds18b20", "  probe %d: ROM %016llX", i, roms[i]);
    }

    // Only the first probe is published for now; search order is by ROM ID,
    // so it stays the same probe across reboots. If none was found, roms[0]
    // is 0 and init() fails, which leaves the sensor skipped in the loop.
    Ds18b20 water(onewire, roms[0]);
    water.init();

    uint32_t seq = 0;
    char payload[128];

    while (true) {
        auto drc = dht.sample();

        if (drc == DhtSensor::ReturnCode::DHT_OK) {
            auto d = dht.read_data();
            ESP_LOGI("dht", "%.1f F  %.1f %%RH", c_to_f(d.temperature), d.humidity);

            snprintf(payload, sizeof(payload),
                     "{\"schema\":1,\"seq\":%lu,\"id\":\"ENC_T\",\"v\":%.2f}",
                     seq++, d.temperature);
            if (mqtt.publish(telemetry_topic, payload) != MqttClient::ReturnCode::OK) {
                ESP_LOGW("main", "publish ENC_T failed");
            }

            snprintf(payload, sizeof(payload),
                     "{\"schema\":1,\"seq\":%lu,\"id\":\"ENC_RH\",\"v\":%.2f}",
                     seq++, d.humidity);
            if (mqtt.publish(telemetry_topic, payload) != MqttClient::ReturnCode::OK) {
                ESP_LOGW("main", "publish ENC_RH failed");
            }
        } else {
            ESP_LOGW("dht", "sample failed, rc=%d", static_cast<int>(drc));
        }

        if (water.get_status() != Ds18b20::Status::UNINITIALIZED) {
            auto wrc = water.sample();

            if (wrc == Ds18b20::ReturnCode::OK) {
                auto w = water.read_data();
                ESP_LOGI("ds18b20", "water %.2f C (%.1f F)", w.temperature, c_to_f(w.temperature));

                snprintf(payload, sizeof(payload),
                         "{\"schema\":1,\"seq\":%lu,\"id\":\"WATER_T\",\"v\":%.2f}",
                         seq++, w.temperature);
                if (mqtt.publish(telemetry_topic, payload) != MqttClient::ReturnCode::OK) {
                    ESP_LOGW("main", "publish WATER_T failed");
                }
            } else {
                ESP_LOGW("ds18b20", "sample failed, rc=%d", static_cast<int>(wrc));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DHT_READ_INTERVAL_MS));
    }
}