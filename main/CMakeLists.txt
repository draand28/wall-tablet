idf_component_register(SRCS "main.c" "ui.c" "camera.c" "ha.c" "ota.c" "log_server.c"
                    REQUIRES nvs_flash esp_wifi esp_netif lwip esp_timer
                             esp_http_client esp_http_server app_update
                             json esp_jpeg esp_driver_jpeg
                             esp_hw_support bsp_i2c bsp_display bsp_wifi
                    INCLUDE_DIRS ".")
