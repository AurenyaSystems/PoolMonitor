#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "dht_sensor.h"
#include "ds18b20.h"
#include "ezo_sensor.h"
#include "mqtt.h"
#include "seq_counter.h"

// Telemetry publisher (POOL-36). Sampling and transmission run on independent
// schedules: each sensor's sampler pushes readings into a queue at its own
// interval, and the transmitter drains the queue on its own cadence. Nothing
// assumes sensors sample together, and radio activity is not tied to sample
// time, which leaves room to stagger power use once POOL-44 has numbers.

/**
 * @brief One raw reading, as it will be published (POOL-38).
 */
struct Reading
{
    char id[12];        ///< sensor ID, e.g. "WATER_T"; a copy, so it can be persisted later (POOL-3)
    int64_t ts;         ///< epoch seconds when the reading was taken
    float v;            ///< value in SI units
};

/**
 * @brief Bounded in-memory queue of readings between samplers and the transmitter.
 *
 * Phase 1 has no persistent buffering: when full, the oldest reading is
 * dropped. POOL-3 replaces this with a persistent store behind the same
 * push/pop, so samplers and transmitter don't change.
 */
class ReadingQueue
{
private:
    QueueHandle_t _queue;
    uint32_t _dropped;          ///< readings dropped because the queue was full

    constexpr static const char* TAG = "[QUEUE]";

public:
    ReadingQueue();

    /**
     * @brief Allocate the queue.
     * @param depth Maximum number of readings held
     * @return true if successful
     */
    bool init(size_t depth);

    /**
     * @brief Add a reading. Never blocks; drops the oldest reading if full.
     * @param id Sensor ID (truncated to fit Reading::id)
     * @param ts Epoch seconds when the reading was taken
     * @param v Value in SI units
     */
    void push(const char* id, int64_t ts, float v);

    /**
     * @brief Take the oldest reading, without blocking.
     * @param out Receives the reading
     * @return true if a reading was taken, false if the queue is empty
     */
    bool pop(Reading& out);

    size_t count() const;                               ///< readings waiting
    uint32_t get_dropped() const { return _dropped; }   ///< readings dropped since boot

    ReadingQueue(const ReadingQueue&) = delete;
    ReadingQueue& operator=(const ReadingQueue&) = delete;
};

/**
 * @brief Serialises access to the sensors between the samplers and the CLI.
 *
 * Each driver is a multi-step bus conversation (write, wait, read), so two
 * tasks talking to the same bus at once would corrupt both. One lock for all
 * sensors keeps it simple; a command waits at most one sample (~1 s).
 *
 * Usage: `SensorLock lock;` at the top of a scope that touches a sensor.
 */
class SensorLock
{
private:
    static SemaphoreHandle_t _mutex;
public:
    SensorLock();
    ~SensorLock();
    SensorLock(const SensorLock&) = delete;
    SensorLock& operator=(const SensorLock&) = delete;
};

/**
 * @brief Arguments for a sampler task. Must outlive the task.
 */
template <typename Sensor>
struct SamplerArgs
{
    Sensor* sensor;
    ReadingQueue* queue;
    uint32_t interval_ms;
};

/**
 * @brief Arguments for an EZO sampler task. Must outlive the task.
 */
struct EzoSamplerArgs
{
    EzoSensor* sensor;
    ReadingQueue* queue;
    uint32_t interval_ms;
    const char* id;             ///< sensor ID to publish under, "PH" or "ORP"
    Ds18b20* water;             ///< water probe for temperature compensation, or nullptr
};

/**
 * @brief FreeRTOS task: sample the DHT every interval_ms, queue ENC_T and ENC_RH.
 * @param arg SamplerArgs<DhtSensor>*
 */
void dht_sampler_task(void* arg);

/**
 * @brief FreeRTOS task: sample the water probe every interval_ms, queue WATER_T.
 * @param arg SamplerArgs<Ds18b20>*
 */
void water_sampler_task(void* arg);

/**
 * @brief FreeRTOS task: sample an EZO stamp every interval_ms, queue it under args->id.
 *
 * If a water probe is given, its last good temperature is sent to the stamp
 * before each reading for compensation (pH).
 *
 * @param arg EzoSamplerArgs*
 */
void ezo_sampler_task(void* arg);

/**
 * @brief Drain the queue to MQTT every interval_ms. Never returns.
 *
 * Publishes one flat message per reading, QoS 1, not retained, and assigns seq
 * at publish time. While the broker is unreachable the readings stay queued
 * (oldest dropped when full) instead of being popped and lost.
 *
 * @param queue Queue the samplers push into
 * @param mqtt Connected (or reconnecting) MQTT client
 * @param seq Persistent sequence counter
 * @param topic Telemetry topic, pool/<device_id>/telemetry
 * @param interval_ms Transmit cadence
 */
void run_transmitter(ReadingQueue& queue, MqttClient& mqtt, SeqCounter& seq,
                     const char* topic, uint32_t interval_ms);

#endif // TELEMETRY_H
