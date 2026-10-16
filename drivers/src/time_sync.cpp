#include "time_sync.h"

#include <atomic>
#include <ctime>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

namespace
{
    constexpr const char* TAG = "[TIME]";

    // Set explicitly rather than left at the IDF default. In light sleep the
    // clock runs off the RTC slow clock (internal RC oscillator on the devkit),
    // which drifts noticeably, so resync more often than the hourly default.
    // One small UDP exchange on an already-associated link is cheap.
    constexpr uint32_t SYNC_INTERVAL_MS = 15 * 60 * 1000;

    std::atomic<bool> synced{false};

    // Runs in the lwIP task on every successful sync
    void on_sync(struct timeval* tv)
    {
        char utc[32];
        struct tm t;
        gmtime_r(&tv->tv_sec, &t);
        strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &t);

        if (!synced.exchange(true))
        {
            // Log lines before this point are stamped with ms since boot.
            // Pairing the two lets them be placed on the real timeline.
            ESP_LOGI(TAG, "first sync: %s at uptime %lld ms", utc, esp_timer_get_time() / 1000);
        }
        else
        {
            ESP_LOGD(TAG, "resync: %s", utc);
        }
    }
}

esp_err_t time_sync_start(const char* server)
{
    esp_sntp_set_sync_interval(SYNC_INTERVAL_MS);   // stored, applied when the client starts

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    cfg.sync_cb = on_sync;

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "sntp init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "sntp started, server %s, resync every %lu s", server, SYNC_INTERVAL_MS / 1000);
    return ESP_OK;
}

bool time_sync_wait(uint32_t timeout_ms)
{
    if (synced) { return true; }
    return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
}

bool time_is_synced()
{
    return synced;
}
