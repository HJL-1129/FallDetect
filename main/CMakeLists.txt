idf_component_register(SRCS "wifi_upload.c" "fall_data.c" "main.c" "dns_server.c" "scan.c" "ota.c" "mpu6500.c" "gsm4g.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES nvs_flash esp_wifi esp_http_client esp_http_server
                                  esp_netif esp_event driver esp_adc json
                                  app_update esp_partition lwip
                    EMBED_FILES root.html
                    )
