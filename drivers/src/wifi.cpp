#include "wifi.h"

#include <algorithm>
#include <cstring>
#include "esp_log.h"
#include "esp_wifi.h"

namespace {
    constexpr uint32_t WIFI_CONNECTED_BIT   = BIT0;
    constexpr uint32_t WIFI_FAILED_BIT      = BIT1;

    // Reconnect backoff (POOL-33): 1 s doubling to a 60 s cap, reset once an
    // IP is obtained. This is the only retry loop in the connectivity stack;
    // esp-mqtt reconnects on its own once the link is back.
    constexpr uint32_t INITIAL_BACKOFF_MS   = 1000;
    constexpr uint32_t MAX_BACKOFF_MS       = 60000;

    // Bad credentials never fix themselves, so stop after this many auth-type
    // failures without a successful association in between. More than one,
    // since an AP that is still booting can fail a handshake or two.
    constexpr uint32_t AUTH_FAIL_LIMIT      = 5;

    bool is_auth_failure(uint8_t reason)
    {
        return reason == WIFI_REASON_AUTH_FAIL ||
               reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
               reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
    }
}

Wifi::Wifi(const char* ssid, const char* password):
    _status(Status::UNINITIALIZED),
    _last_reason(0),
    _backoff_ms(INITIAL_BACKOFF_MS),
    _auth_failures(0),
    _disconnects(0),
    _retry_timer(nullptr),
    _events(nullptr)
{
    if(ssid) {strncpy(_ssid, ssid, sizeof(_ssid) - 1);}
    if(password) {strncpy(_password, password, sizeof(_password) - 1);}
}

void Wifi::event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    auto* self = static_cast<Wifi*>(arg);

    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) 
        esp_wifi_connect();

    else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        auto* ev = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        self->_last_reason = ev->reason;
        self->_disconnects++;

        xEventGroupClearBits(self->_events, WIFI_CONNECTED_BIT);

        if (is_auth_failure(ev->reason) && ++self->_auth_failures >= AUTH_FAIL_LIMIT)
        {
            self->_status = Status::FAILED;
            ESP_LOGE(TAG, "disconnected (reason %d), giving up after %lu auth failures: check credentials",
                     ev->reason, self->_auth_failures);
            xEventGroupSetBits(self->_events, WIFI_FAILED_BIT);
            return;
        }

        self->_status = Status::DEGRADED;
        ESP_LOGW(TAG, "disconnected (reason %d, %lu since boot), retry in %lu ms",
                 ev->reason, self->_disconnects, self->_backoff_ms);
        self->schedule_retry();
    }
    else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        auto* ev = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));

        self->_backoff_ms = INITIAL_BACKOFF_MS;
        self->_auth_failures = 0;
        self->_status = Status::READY;

        xEventGroupClearBits(self->_events, WIFI_FAILED_BIT);
        xEventGroupSetBits(self->_events, WIFI_CONNECTED_BIT);
    }
}

void Wifi::schedule_retry()
{
    // The event handler must not block, so the wait happens on a timer
    esp_err_t err = esp_timer_start_once(_retry_timer, static_cast<uint64_t>(_backoff_ms) * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "retry timer start failed: %s", esp_err_to_name(err));
    }
    _backoff_ms = std::min(_backoff_ms * 2, MAX_BACKOFF_MS);
}

void Wifi::retry_cb(void* arg)
{
    auto* self = static_cast<Wifi*>(arg);

    // A failed attempt normally comes back as a DISCONNECTED event, which
    // schedules the next one. If the call itself fails there is no event, so
    // reschedule here or the retry chain would stop.
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "connect failed to start: %s, retry in %lu ms", esp_err_to_name(err), self->_backoff_ms);
        self->schedule_retry();
    }
}

Wifi::ReturnCode Wifi::init()
{
    if (_status != Status::UNINITIALIZED) { return ReturnCode::ALREADY_INITIALIZED; }
    if (_ssid[0] == '\0')                 { return ReturnCode::INVALID_ARG; }

    _events = xEventGroupCreate();
    if (!_events)
    {
        ESP_LOGE(TAG, "event group alloc failed");
        _status = Status::FAILED;
        return ReturnCode::INTERNAL_ERROR;
    }

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &Wifi::retry_cb;
    timer_args.arg = this;
    timer_args.name = "wifi_retry";
    if (esp_timer_create(&timer_args, &_retry_timer) != ESP_OK)
    {
        ESP_LOGE(TAG, "retry timer alloc failed");
        vEventGroupDelete(_events); _events = nullptr;
        _status = Status::FAILED;
        return ReturnCode::INTERNAL_ERROR;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        vEventGroupDelete(_events); _events = nullptr;
        _status = Status::FAILED;
        return ReturnCode::INTERNAL_ERROR;
    }

    if ((err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   &Wifi::event_handler, this, nullptr)) != ESP_OK ||
        (err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &Wifi::event_handler, this, nullptr)) != ESP_OK)
    {
        ESP_LOGE(TAG, "event handler register failed: %s", esp_err_to_name(err));
        vEventGroupDelete(_events); _events = nullptr;
        _status = Status::FAILED;
        return ReturnCode::INTERNAL_ERROR;
    }

    wifi_config_t sta_config = {};
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.ssid), _ssid, sizeof(sta_config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(sta_config.sta.password), _password, sizeof(sta_config.sta.password));

    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_set_config(WIFI_IF_STA, &sta_config)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi config/start failed: %s", esp_err_to_name(err));
        vEventGroupDelete(_events); _events = nullptr;
        _status = Status::FAILED;
        return ReturnCode::INTERNAL_ERROR;
    }

    _status = Status::DEGRADED;
    ESP_LOGI(TAG, "WiFi initialization complete, connecting to SSID: %s", _ssid);
    return ReturnCode::OK;
}

Wifi::ReturnCode Wifi::wait_for_connection(uint32_t timeout_ms)
{
    if (!_events) { return ReturnCode::NOT_INITIALIZED; }

    EventBits_t bits = xEventGroupWaitBits(
        _events, 
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT, 
        pdFALSE, // do not clear bits on exit
        pdFALSE, // wait for any bit
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) { return ReturnCode::OK; }

    // FAILED is only set for repeated auth failures; anything else keeps retrying
    if (bits & WIFI_FAILED_BIT) { return ReturnCode::AUTH_FAILED; }
    if (_last_reason == WIFI_REASON_NO_AP_FOUND) { return ReturnCode::AP_NOT_FOUND; }
    return ReturnCode::TIMEOUT;
}

bool Wifi::has_ip() const
{
    if(!_events) { return false; }
    return (xEventGroupGetBits(_events) & WIFI_CONNECTED_BIT) != 0;
}


Wifi::ReturnCode Wifi::set_power_save(bool enable)
{
    if (_status == Status::UNINITIALIZED) { return ReturnCode::NOT_INITIALIZED; }

    esp_err_t err = esp_wifi_set_ps(enable ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set power save failed: %s", esp_err_to_name(err));
        return ReturnCode::INTERNAL_ERROR;
    }
    ESP_LOGI(TAG, "modem sleep %s", enable ? "enabled" : "disabled");
    return ReturnCode::OK;
}