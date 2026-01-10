idf_component_register(SRCS "main.c"
                        PRIV_REQUIRES nvs_flash esp_driver_gpio esp_event esp_netif esp_wifi
                       INCLUDE_DIRS ".")
