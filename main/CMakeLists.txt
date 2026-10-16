idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        app
        drivers
        nvs_flash
        esp_netif
        esp_event
        esp_pm
        esp_app_format
)