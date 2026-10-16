#include "secrets.h"

//drivers
#include "dht_sensor.h"
#include "wifi.h"
#include "mqtt.h"

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

    uint32_t seq = 0;
    char payload[128];

    while (true) {
        auto drc = dht.sample();

        if (drc == DhtSensor::ReturnCode::DHT_OK) {
            auto d = dht.read_data();
            ESP_LOGI("dht", "%.1f F  %.1f %%RH", c_to_f(d.temperature), d.humidity);

            snprintf(payload, sizeof(payload),
                     "{\"schema\":1,\"seq\":%lu,\"id\":\"TMP\",\"v\":%.2f}",
                     seq++, d.temperature);
            if (mqtt.publish(telemetry_topic, payload) != MqttClient::ReturnCode::OK) {
                ESP_LOGW("main", "publish TMP failed");
            }

            snprintf(payload, sizeof(payload),
                     "{\"schema\":1,\"seq\":%lu,\"id\":\"HUM\",\"v\":%.2f}",
                     seq++, d.humidity);
            if (mqtt.publish(telemetry_topic, payload) != MqttClient::ReturnCode::OK) {
                ESP_LOGW("main", "publish HUM failed");
            }
        } else {
            ESP_LOGW("dht", "sample failed, rc=%d", static_cast<int>(drc));
        }

        vTaskDelay(pdMS_TO_TICKS(DHT_READ_INTERVAL_MS));
    }
}