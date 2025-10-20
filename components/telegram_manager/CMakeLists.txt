idf_component_register(SRCS "telegram.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_client persistence esp_crt_bundle deepsleep_manager esp_netif mbedtls)
