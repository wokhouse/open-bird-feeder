idf_component_register(SRCS "main.c" "zone_motion.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES espressif__esp32-camera esp_wifi esp_netif esp_event
                                     esp_http_client esp_timer nvs_flash)
