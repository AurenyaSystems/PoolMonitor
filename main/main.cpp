#include "dht_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// constexpr gpio_num_t GPIO_NUM_2 = 2; // GPIO pin for the LED
constexpr float c_to_f(float c) { return c * 9.0f / 5.0f + 32.0f; }

extern "C" void app_main() {
    gpio_reset_pin(GPIO_NUM_2);
    gpio_set_direction(GPIO_NUM_2   , GPIO_MODE_OUTPUT);
    DhtSensor dht(GPIO_NUM_4, DhtSensor::DHT22);
    ESP_ERROR_CHECK(dht.init());

    while (true) {
        auto rc = dht.sample();
        if (rc == DhtSensor::ReturnCode::DHT_OK) {
            auto d = dht.read_data();
            ESP_LOGI("dht", "%.1f F  %.1f %%RH", c_to_f(d.temperature), d.humidity);
            gpio_set_level(GPIO_NUM_2   , 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            gpio_set_level(GPIO_NUM_2   , 0);
            vTaskDelay(pdMS_TO_TICKS(1500));
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
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
}