
#include "secrets.h"

//drivers
#include "dht_sensor.h"
#include "wifi.h"

#include "nvs_flash.h"
#include "esp_netif.h"


#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// constexpr gpio_num_t GPIO_NUM_2 = 2; // GPIO pin for the LED
constexpr float c_to_f(float c) { return c * 9.0f / 5.0f + 32.0f; }

extern "C" void app_main() {

    //general setup
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // wifi stuff
    constexpr const int RETRY_TIMER = 15000; 
    Wifi wifi(WIFI_SSID, WIFI_PASSWORD);
    auto rc = wifi.init();
    if (rc != Wifi::ReturnCode::OK) {
        ESP_LOGE("wifi", "wifi init failed: %d", static_cast<int>(rc));
        return;
    }   


    // dht sensor stuff
    constexpr const uint64_t DHT_READ_INTERVAL_MS = 59000; 
    gpio_reset_pin(GPIO_NUM_2);
    gpio_set_direction(GPIO_NUM_2   , GPIO_MODE_OUTPUT);
    DhtSensor dht(GPIO_NUM_4, DhtSensor::DHT22);
    ESP_ERROR_CHECK(dht.init());

    rc = wifi.wait_for_connection(RETRY_TIMER);
    if (rc == Wifi::ReturnCode::OK) {
        ESP_LOGI("main", "wifi connected");
    } else {
        ESP_LOGW("main", "wifi not connected, rc=%d", static_cast<int>(rc));
    }

    while (true) {
        auto rc = dht.sample();
        if (rc == DhtSensor::ReturnCode::DHT_OK) {
            auto d = dht.read_data();
            ESP_LOGI("dht", "%.1f F  %.1f %%RH", c_to_f(d.temperature), d.humidity);
            gpio_set_level(GPIO_NUM_2   , 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            gpio_set_level(GPIO_NUM_2   , 0);
            vTaskDelay(pdMS_TO_TICKS(DHT_READ_INTERVAL_MS));
        } else {
            ESP_LOGW("dht", "rc=%d", static_cast<int>(rc));
            gpio_set_level(GPIO_NUM_2   , 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(GPIO_NUM_2   , 0);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(GPIO_NUM_2   , 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(GPIO_NUM_2   , 0);
            vTaskDelay(pdMS_TO_TICKS(50));
            vTaskDelay(pdMS_TO_TICKS(DHT_READ_INTERVAL_MS));
        }
    }
}