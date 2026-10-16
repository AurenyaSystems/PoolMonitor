idf_component_register(
    SRCS
        "src/dht_sensor.cpp"
        "src/wifi.cpp"
        "src/mqtt.cpp"
        "src/ds18b20.cpp"
    INCLUDE_DIRS "inc"
    REQUIRES
        esp_driver_gpio
        esp_timer
        esp_event
        core
    PRIV_REQUIRES
        esp_wifi
        esp_netif
        mqtt
)