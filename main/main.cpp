#include "secrets.h"

//app
#include "telemetry.h"

//drivers
#include "dht_sensor.h"
#include "wifi.h"
#include "mqtt.h"
#include "ds18b20.h"
#include "ezo_sensor.h"
#include "time_sync.h"
#include "seq_counter.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_mac.h"

#include <cstdio>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// LAN-only setups can point this at the router or broker host in secrets.h
#ifndef SNTP_SERVER
#define SNTP_SERVER "pool.ntp.org"
#endif

// device_id is the topic prefix and MQTT client id (POOL-38). It comes from
// the factory MAC so every board is unique without configuration. Defining
// MQTT_DEVICE_ID in secrets.h overrides it, e.g. to keep an existing id.
static void get_device_id(char* out, size_t len)
{
#ifdef MQTT_DEVICE_ID
    snprintf(out, len, "%s", MQTT_DEVICE_ID);
#else
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(out, len, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
}

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
    char device_id[32];
    get_device_id(device_id, sizeof(device_id));
    ESP_LOGI("main", "device id: %s", device_id);

    char status_topic[96];
    char telemetry_topic[96];
    snprintf(status_topic, sizeof(status_topic), "pool/%s/status", device_id);
    snprintf(telemetry_topic, sizeof(telemetry_topic), "pool/%s/telemetry", device_id);

    MqttClient mqtt(MQTT_BROKER_URI, device_id);
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
    DhtSensor dht(GPIO_NUM_4, DhtSensor::DHT22);
    ESP_ERROR_CHECK(dht.init());

    // water temperature: one DS18B20 on its own 1-Wire bus (POOL-9)
    constexpr gpio_num_t ONEWIRE_PIN = GPIO_NUM_27;
    OneWire onewire(ONEWIRE_PIN);
    ESP_ERROR_CHECK(onewire.init());

    // Find the probe's ROM ID instead of hardcoding it, so a swapped probe
    // needs no reflash. If none is found, rom stays 0 and init() fails, so no
    // water sampler is started.
    uint64_t rom = 0;
    if (onewire.search(&rom, 1) == 1) {
        ESP_LOGI("ds18b20", "probe on GPIO %d: ROM %016llX", ONEWIRE_PIN, rom);
    } else {
        ESP_LOGE("ds18b20", "no probe found on GPIO %d", ONEWIRE_PIN);
    }
    Ds18b20 water(onewire, rom);
    water.init();

    // I2C bus for the EZO stamps (POOL-10). Internal pull-ups are weak; the
    // EZO carriers or external 4.7k resistors should provide the real ones.
    constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_21;
    constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_22;
    i2c_master_bus_config_t i2c_cfg = {};
    i2c_cfg.i2c_port = -1;   // auto
    i2c_cfg.sda_io_num = I2C_SDA_PIN;
    i2c_cfg.scl_io_num = I2C_SCL_PIN;
    i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_cfg.glitch_ignore_cnt = 7;
    i2c_cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    // pH stamp: range is the EZO-pH measurement range
    constexpr uint8_t EZO_PH_ADDR = 0x63;
    EzoSensor ph(i2c_bus, EZO_PH_ADDR, 0.0f, 14.0f);
    ph.init();

    // ORP stamp (POOL-11): range is the EZO-ORP measurement range, in mV.
    // No temperature compensation; ORP stamps don't take one.
    constexpr uint8_t EZO_ORP_ADDR = 0x62;
    EzoSensor orp(i2c_bus, EZO_ORP_ADDR, -1019.9f, 1019.9f);
    orp.init();

    // telemetry (POOL-36): each sensor samples on its own interval and queues
    // readings; this task drains the queue to MQTT on its own cadence.
    constexpr const uint32_t DHT_INTERVAL_MS   = 30 * 60 * 1000;
    constexpr const uint32_t WATER_INTERVAL_MS = 30 * 60 * 1000;
    constexpr const uint32_t PH_INTERVAL_MS    = 30 * 60 * 1000;
    constexpr const uint32_t ORP_INTERVAL_MS   = 30 * 60 * 1000;
    constexpr const uint32_t TX_INTERVAL_MS    = 60 * 1000;
    constexpr const size_t   QUEUE_DEPTH       = 64;    // ~6 h of readings at these intervals if the broker is down

    ReadingQueue queue;
    if (!queue.init(QUEUE_DEPTH)) { return; }

    SeqCounter seq;
    seq.init();   // on failure it still counts, just not across reboots

    // Samplers run on core 1 like this task: the DHT and 1-Wire bit loops
    // disable interrupts briefly and must stay off the WiFi core (0).
    // Their args live on this stack, which is fine since run_transmitter never returns.
    SamplerArgs<DhtSensor> dht_args { &dht, &queue, DHT_INTERVAL_MS };
    xTaskCreatePinnedToCore(dht_sampler_task, "dht", 4096, &dht_args, 5, nullptr, 1);

    SamplerArgs<Ds18b20> water_args { &water, &queue, WATER_INTERVAL_MS };
    if (water.get_status() != Ds18b20::Status::UNINITIALIZED) {
        xTaskCreatePinnedToCore(water_sampler_task, "water", 4096, &water_args, 5, nullptr, 1);
    }

    // pH gets the water probe for temperature compensation, if there is one
    Ds18b20* ph_water = (water.get_status() != Ds18b20::Status::UNINITIALIZED) ? &water : nullptr;
    EzoSamplerArgs ph_args { &ph, &queue, PH_INTERVAL_MS, "PH", ph_water };
    if (ph.get_status() != EzoSensor::Status::UNINITIALIZED) {
        xTaskCreatePinnedToCore(ezo_sampler_task, "ph", 4096, &ph_args, 5, nullptr, 1);
    }

    EzoSamplerArgs orp_args { &orp, &queue, ORP_INTERVAL_MS, "ORP", nullptr };
    if (orp.get_status() != EzoSensor::Status::UNINITIALIZED) {
        xTaskCreatePinnedToCore(ezo_sampler_task, "orp", 4096, &orp_args, 5, nullptr, 1);
    }

    run_transmitter(queue, mqtt, seq, telemetry_topic, TX_INTERVAL_MS);
}
