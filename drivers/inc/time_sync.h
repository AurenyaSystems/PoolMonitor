#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdint.h>         // for uint32_t
#include "esp_err.h"        // for esp_err_t

// Wall-clock time from SNTP, used to timestamp readings (POOL-37).
// Plain functions rather than a class: lwIP's SNTP client is a single global
// service, so there is nothing to instantiate.

/**
 * @brief Start the SNTP client and set an explicit resync interval.
 *
 * Safe to call before WiFi has an IP; the first sync happens once the network
 * is up. The system clock is UTC.
 *
 * @param server NTP server hostname or IP. The router or broker host works, no
 *  internet access needed.
 * @return ESP_OK if successful, otherwise the esp_netif_sntp_init error
 */
esp_err_t time_sync_start(const char* server);

/**
 * @brief Block until the first sync completes or the timeout expires.
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return true if the clock has been synced
 */
bool time_sync_wait(uint32_t timeout_ms);

/**
 * @brief Whether SNTP has set the clock at least once since boot.
 *
 * Until it has, time() is not a real epoch and readings must not be published
 * with it.
 *
 * @return true once synced
 */
bool time_is_synced();

#endif // TIME_SYNC_H
