#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

class MqttClient
{
public:
    enum class Status { UNINITIALIZED, READY, DEGRADED, FAILED };
    enum class ReturnCode
    {
        OK,
        INVALID_ARG,        ///< empty URI or device id
        NOT_INITIALIZED,    ///< called before init()
        ALREADY_INITIALIZED,
        TIMEOUT,            ///< no CONNACK within the requested window
        NOT_CONNECTED,      ///< publish attempted while offline
        PUBLISH_FAILED,     ///< enqueue rejected (outbox full, bad args)
        INTERNAL_ERROR      ///< an underlying esp_ call failed
    };

private:
    char _broker_uri[128] {};   ///< e.g. mqtt://192.168.4.47:1883
    char _device_id[32]   {};   ///< topic prefix and client id
    char _topic_buf[96]   {};   ///< scratch for building topic strings

    void* _client;              ///< esp_mqtt_client_handle_t, opaque here
    EventGroupHandle_t _events; ///< connection state bits
    Status _status;
    uint32_t _seq;              ///< monotonic, RAM-only for now (POOL-38 wants NVS)

    constexpr static const char* TAG = "[MQTT]";

    static void event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data);

public:
    /**
     * @brief Constructor for the MqttClient class
     * @param broker_uri Broker URI, e.g. "mqtt://192.168.4.47:1883"
     * @param device_id Device identifier used as client id and topic prefix
     */
    MqttClient(const char* broker_uri, const char* device_id);

    /**
     * @brief Initialize and start the MQTT client
     * @return ReturnCode indicating the result of the operation
     *
     * Non-blocking: returns once the connect attempt is under way. Requires
     * an active network connection and the default event loop to exist.
     */
    ReturnCode init();

    /**
     * @brief Wait for the broker connection to be established
     * @param timeout_ms Maximum time to wait in milliseconds
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode wait_for_connection(uint32_t timeout_ms);

    /**
     * @brief Publish a payload to a topic
     * @param topic Full topic string
     * @param payload Message body
     * @param qos Quality of service (0, 1, or 2)
     * @param retain Whether the broker should retain this message
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode publish(const char* topic, const char* payload, int qos = 1, bool retain = false);

    /**
     * @brief Check whether the client is currently connected to the broker
     * @return true if connected, false otherwise
     */
    bool is_connected() const;

    /**
     * @brief Get the current status of the MQTT client
     * @return Status indicating the current state
     */
    Status get_status() const { return _status; }

    MqttClient(const MqttClient&)            = delete;
    MqttClient& operator=(const MqttClient&) = delete;
    MqttClient(MqttClient&&)                 = delete;
    MqttClient& operator=(MqttClient&&)      = delete;
};

#endif // MQTT_CLIENT_H