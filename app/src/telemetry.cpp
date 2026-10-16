#include "telemetry.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "freertos/task.h"

#include "time_sync.h"

namespace
{
    constexpr float c_to_f(float c) { return c * 9.0f / 5.0f + 32.0f; }

    // Readings are only queued with a real epoch: a bogus timestamp looks
    // valid downstream, a missing reading is just a gap (POOL-37). Sensors
    // are still sampled while unsynced so their health stays current.
    bool can_queue(const char* tag)
    {
        if (time_is_synced()) { return true; }
        ESP_LOGW(tag, "time not synced, reading not queued");
        return false;
    }
}

// ---------------------------------------------------------------- queue

ReadingQueue::ReadingQueue():
    _queue(nullptr),
    _dropped(0)
{
}

bool ReadingQueue::init(size_t depth)
{
    _queue = xQueueCreate(depth, sizeof(Reading));
    if (!_queue)
    {
        ESP_LOGE(TAG, "queue alloc failed");
        return false;
    }
    return true;
}

void ReadingQueue::push(const char* id, int64_t ts, float v)
{
    Reading r {};
    strncpy(r.id, id, sizeof(r.id) - 1);
    r.ts = ts;
    r.v = v;

    if (xQueueSend(_queue, &r, 0) == pdTRUE) { return; }

    // Full: make room by dropping the oldest. Another sampler could refill
    // the slot in between, in which case this reading is the one dropped.
    Reading oldest;
    if (xQueueReceive(_queue, &oldest, 0) == pdTRUE)
    {
        _dropped++;
        ESP_LOGW(TAG, "full, dropped %s from %lld (%lu dropped so far)",
                 oldest.id, static_cast<long long>(oldest.ts), _dropped);
    }
    if (xQueueSend(_queue, &r, 0) != pdTRUE)
    {
        _dropped++;
        ESP_LOGW(TAG, "full, dropped %s", r.id);
    }
}

bool ReadingQueue::pop(Reading& out)
{
    return xQueueReceive(_queue, &out, 0) == pdTRUE;
}

// ---------------------------------------------------------------- samplers

void dht_sampler_task(void* arg)
{
    auto* a = static_cast<SamplerArgs<DhtSensor>*>(arg);

    while (true)
    {
        auto rc = a->sensor->sample();
        int64_t ts = time(nullptr);

        // A reading only exists when sample() succeeded, which also means the
        // sensor is READY: DEGRADED/FAILED sensors produce nothing, never a null
        if (rc == DhtSensor::ReturnCode::DHT_OK)
        {
            auto d = a->sensor->read_data();
            ESP_LOGI("dht", "%.1f F  %.1f %%RH", c_to_f(d.temperature), d.humidity);

            if (can_queue("dht"))
            {
                a->queue->push("ENC_T", ts, d.temperature);
                a->queue->push("ENC_RH", ts, d.humidity);
            }
        }
        else
        {
            ESP_LOGW("dht", "sample failed, rc=%d", static_cast<int>(rc));
        }

        vTaskDelay(pdMS_TO_TICKS(a->interval_ms));
    }
}

void water_sampler_task(void* arg)
{
    auto* a = static_cast<SamplerArgs<Ds18b20>*>(arg);

    while (true)
    {
        auto rc = a->sensor->sample();
        int64_t ts = time(nullptr);

        if (rc == Ds18b20::ReturnCode::OK)
        {
            auto w = a->sensor->read_data();
            ESP_LOGI("ds18b20", "water %.2f C (%.1f F)", w.temperature, c_to_f(w.temperature));

            if (can_queue("ds18b20"))
            {
                a->queue->push("WATER_T", ts, w.temperature);
            }
        }
        else
        {
            ESP_LOGW("ds18b20", "sample failed, rc=%d", static_cast<int>(rc));
        }

        vTaskDelay(pdMS_TO_TICKS(a->interval_ms));
    }
}

void ezo_sampler_task(void* arg)
{
    auto* a = static_cast<EzoSamplerArgs*>(arg);

    while (true)
    {
        // Temperature compensation happens on the stamp; firmware only passes
        // the measured water temperature along. If there is none yet, the
        // stamp keeps its last value (25 degC after power-up).
        if (a->water)
        {
            float t = a->water->read_data().temperature;
            if (std::isnan(t)) {
                ESP_LOGW(a->id, "no water temperature yet, reading uncompensated");
            } else if (a->sensor->set_temperature(t) != EzoSensor::ReturnCode::OK) {
                ESP_LOGW(a->id, "temperature compensation failed");
            }
        }

        auto rc = a->sensor->sample();
        int64_t ts = time(nullptr);

        if (rc == EzoSensor::ReturnCode::OK)
        {
            float v = a->sensor->read_data().value;
            ESP_LOGI(a->id, "%.2f", v);

            if (can_queue(a->id))
            {
                a->queue->push(a->id, ts, v);
            }
        }
        else
        {
            ESP_LOGW(a->id, "sample failed, rc=%d", static_cast<int>(rc));
        }

        vTaskDelay(pdMS_TO_TICKS(a->interval_ms));
    }
}

// ---------------------------------------------------------------- transmitter

void run_transmitter(ReadingQueue& queue, MqttClient& mqtt, SeqCounter& seq,
                     const char* topic, uint32_t interval_ms)
{
    char payload[128];

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(interval_ms));

        // Leave readings queued while offline rather than popping and losing them
        if (!mqtt.is_connected()) { continue; }

        Reading r;
        while (queue.pop(r))
        {
            // seq is assigned here, at publish time: a reading that fails to
            // publish leaves a gap the server can see
            snprintf(payload, sizeof(payload),
                     "{\"schema\":1,\"seq\":%lu,\"id\":\"%s\",\"ts\":%lld,\"v\":%.2f}",
                     seq.next(), r.id, static_cast<long long>(r.ts), r.v);

            if (mqtt.publish(topic, payload) != MqttClient::ReturnCode::OK)
            {
                ESP_LOGW("telemetry", "publish %s failed", r.id);
            }
        }
    }
}
