#ifndef WIFI_H
#define WIFI_H

#include <stdint.h>         // for uintx_t
#include "esp_event.h"      // for esp_event_base_t
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

class Wifi 
{
public: 
    enum class Status { UNINITIALIZED, READY, DEGRADED, FAILED };
    enum class ReturnCode 
    {
        OK,
        INVALID_ARG,          ///< SSID or password empty / too long
        NOT_INITIALIZED,      ///< called before init()
        ALREADY_INITIALIZED,
        TIMEOUT,              ///< no IP within the requested window
        AUTH_FAILED,          ///< terminal: credentials rejected
        AP_NOT_FOUND,         ///< retryable: SSID not seen
        INTERNAL_ERROR        ///< an underlying esp_ call failed
    };

private:
    // class members
    char _ssid[33] {};          ///< SSID of the WiFi network (max 32 chars + null terminator)
    char _password[65] {};      ///< Password of the WiFi network (max 64 chars + null terminator)

    Status _status;             ///< Current status of the WiFi connection
    uint8_t _last_reason;       ///< Last disconnect reason code
    uint32_t _retry_count;      ///< Count of connection retries

    EventGroupHandle_t _events; ///< Event group handle for WiFi events

    constexpr static const char* TAG = "[WIFI]"; ///< Tag for logging


    // private methods
    static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);


public:
    /**
     * @brief Constructor for the Wifi class
     * @param ssid The SSID of the WiFi network
     * @param password The password of the WiFi network
     */
    Wifi(const char* ssid, const char* password);

    /**
     * @brief Initialize the WiFi connection
     * @return ReturnCode indicating the result of the operation    
     */
    ReturnCode init();

    /**
     * @brief Wait for the WiFi connection to be established
     * @param timeout_ms The maximum time to wait for the connection in milliseconds
     * @return ReturnCode indicating the result of the operation
     */
    ReturnCode wait_for_connection(uint32_t timeout_ms);

    /**
     * @brief Get the current status of the WiFi connection
     * @return Status indicating the current state of the WiFi connection
     */
    Status get_status() const { return _status; }

    /**
     * @brief Check if the device has obtained an IP address
     * @return true if the device has an IP address, false otherwise
     */
    bool has_ip() const;

    Wifi(const Wifi&) = delete;
    Wifi& operator=(const Wifi&) = delete;

};

#endif // WIFI_H