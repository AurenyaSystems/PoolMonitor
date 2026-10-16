#include "mqtt.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "mqtt_client.h"   // IDF's MQTT header

namespace {
    constexpr uint32_t MQTT_CONNECTED_BIT = BIT0;
    constexpr uint32_t MQTT_FAILED_BIT    = BIT1;
}

MqttClient::MqttClient(const char* broker_uri, const char* device_id):
    _client(nullptr),
    _events(nullptr),
    _status(Status::UNINITIALIZED)
{
    if (broker_uri) { strncpy(_broker_uri, broker_uri, sizeof(_broker_uri) - 1); }
    if (device_id)  { strncpy(_device_id,  device_id,  sizeof(_device_id)  - 1); }
}

void MqttClient::event_handler(void* arg, esp_event_base_t /*event_base*/,
                               int32_t event_id, void* event_data)
{
    auto* self = static_cast<MqttClient*>(arg);
    auto* ev   = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (static_cast<esp_mqtt_event_id_t>(event_id))
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected to broker");
            self->_status = Status::READY;
            xEventGroupClearBits(self->_events, MQTT_FAILED_BIT);
            xEventGroupSetBits(self->_events, MQTT_CONNECTED_BIT);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected from broker");
            self->_status = Status::DEGRADED;   // esp-mqtt retries on its own
            xEventGroupClearBits(self->_events, MQTT_CONNECTED_BIT);
            break;

        case MQTT_EVENT_ERROR:
            if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
            {
                ESP_LOGE(TAG, "transport error, sock_errno %d",
                         ev->error_handle->esp_transport_sock_errno);
            }
            else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
            {
                ESP_LOGE(TAG, "connection refused, code %d",
                         ev->error_handle->connect_return_code);
                self->_status = Status::FAILED;
                xEventGroupSetBits(self->_events, MQTT_FAILED_BIT);
            }
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "publish acked, msg_id %d", ev->msg_id);
            break;

        default:
            break;
    }
}

MqttClient::ReturnCode MqttClient::init()
{
    if (_status != Status::UNINITIALIZED) { return ReturnCode::ALREADY_INITIALIZED; }
    if (_broker_uri[0] == '\0' || _device_id[0] == '\0') { return ReturnCode::INVALID_ARG; }

    _events = xEventGroupCreate();
    if (!_events)
    {
        ESP_LOGE(TAG, "event group alloc failed");
        _status = Status::FAILED;
        return ReturnCode::INTERNAL_ERROR;
    }

    // LWT topic must outlive this call, so it lives in a member buffer
    snprintf(_topic_buf, sizeof(_topic_buf), "pool/%s/status", _device_id);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri            = _broker_uri;
    cfg.credentials.client_id         = _device_id;
    cfg.session.last_will.topic       = _topic_buf;
    cfg.session.last_will.msg         = "offline";
    cfg.session.last_will.msg_len     = 7;
    cfg.session.last_will.qos         = 1;
    cfg.session.last_will.retain      = true;
    cfg.session.disable_clean_session = true;   // persistent session
    cfg.session.keepalive             = 300;

    _client = esp_mqtt_client_init(&cfg);
    if (!_client)
    {
        ESP_LOGE(TAG, "client init failed");
        vEventGroupDelete(_events); _events = nullptr;
        _status = Status::FAILED;
        return ReturnCode::INTERNAL_ERROR;
    }

    auto handle = static_cast<esp_mqtt_client_handle_t>(_client);

    esp_err_t err = esp_mqtt_client_register_event(
        handle, MQTT_EVENT_ANY, &MqttClient::event_handler, this);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "event register failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(handle); _client = nullptr;
        vEventGroupDelete(_events); _events = nullptr;
        _status = Status::FAILED;
        return ReturnCode::INTERNAL_ERROR;
    }

    if ((err = esp_mqtt_client_start(handle)) != ESP_OK)
    {
        ESP_LOGE(TAG, "client start failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(handle); _client = nullptr;
        vEventGroupDelete(_events); _events = nullptr;
        _status = Status::FAILED;
        return ReturnCode::INTERNAL_ERROR;
    }

    _status = Status::DEGRADED;   // started, not yet connected
    ESP_LOGI(TAG, "client started, connecting to %s", _broker_uri);
    return ReturnCode::OK;
}

MqttClient::ReturnCode MqttClient::wait_for_connection(uint32_t timeout_ms)
{
    if (!_events) { return ReturnCode::NOT_INITIALIZED; }

    EventBits_t bits = xEventGroupWaitBits(
        _events,
        MQTT_CONNECTED_BIT | MQTT_FAILED_BIT,
        pdFALSE,    // do not clear bits on exit
        pdFALSE,    // wait for any bit
        pdMS_TO_TICKS(timeout_ms));

    if (bits & MQTT_CONNECTED_BIT) { return ReturnCode::OK; }
    if (bits & MQTT_FAILED_BIT)    { return ReturnCode::INTERNAL_ERROR; }
    return ReturnCode::TIMEOUT;
}

MqttClient::ReturnCode MqttClient::publish(const char* topic, const char* payload,
                                           int qos, bool retain)
{
    if (!_client)           { return ReturnCode::NOT_INITIALIZED; }
    if (!topic || !payload) { return ReturnCode::INVALID_ARG; }
    if (!is_connected())    { return ReturnCode::NOT_CONNECTED; }

    auto handle = static_cast<esp_mqtt_client_handle_t>(_client);

    int msg_id = esp_mqtt_client_publish(handle, topic, payload, 0, qos, retain);
    if (msg_id < 0)
    {
        ESP_LOGE(TAG, "publish failed on %s", topic);
        return ReturnCode::PUBLISH_FAILED;
    }
    return ReturnCode::OK;
}

bool MqttClient::is_connected() const
{
    if (!_events) { return false; }
    return (xEventGroupGetBits(_events) & MQTT_CONNECTED_BIT) != 0;
}