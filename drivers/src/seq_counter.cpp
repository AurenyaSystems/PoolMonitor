#include "seq_counter.h"

#include "esp_log.h"

namespace
{
    constexpr const char* NVS_NAMESPACE = "telemetry";
    constexpr const char* NVS_KEY       = "next_seq";
}

SeqCounter::SeqCounter():
    _nvs(0),
    _next(0),
    _persist(false)
{
}

SeqCounter::ReturnCode SeqCounter::init()
{
    if (_persist) { return ReturnCode::ALREADY_INITIALIZED; }

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &_nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs open failed: %s; seq will restart on reboot", esp_err_to_name(err));
        return ReturnCode::INTERNAL_ERROR;
    }

    err = nvs_get_u32(_nvs, NVS_KEY, &_next);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        _next = 0;   // first boot with this flash
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs read failed: %s; seq will restart on reboot", esp_err_to_name(err));
        nvs_close(_nvs);
        return ReturnCode::INTERNAL_ERROR;
    }

    _persist = true;
    ESP_LOGI(TAG, "resuming at seq %lu", _next);
    return ReturnCode::OK;
}

uint32_t SeqCounter::next()
{
    uint32_t seq = _next++;

    if (_persist)
    {
        esp_err_t err = nvs_set_u32(_nvs, NVS_KEY, _next);
        if (err == ESP_OK) { err = nvs_commit(_nvs); }
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "nvs write failed: %s", esp_err_to_name(err));
        }
    }
    return seq;
}
