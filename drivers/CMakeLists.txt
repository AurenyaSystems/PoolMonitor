idf_component_register(
    SRCS "src/dht_sensor.cpp" "src/wifi.cpp"
    INCLUDE_DIRS "inc"
    REQUIRES esp_driver_gpio esp_timer esp_event
    PRIV_REQUIRES esp_wifi  
)