idf_component_register(SRCS "ota_manager.c"
                    INCLUDE_DIRS "include" 
                    REQUIRES esp_http_client esp_https_ota nvs_flash mqtt json app_update mbedtls)
