idf_component_register(SRCS "main.cpp"
                    PRIV_REQUIRES spi_flash
                    REQUIRES drivers core 
                    PRIV_REQUIRES nvs_flash esp_netif
                    INCLUDE_DIRS ".")
