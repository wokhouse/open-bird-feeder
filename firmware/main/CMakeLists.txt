idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES espressif__esp32-camera esp_wifi esp_netif esp_event esp_http_client nvs_flash)
