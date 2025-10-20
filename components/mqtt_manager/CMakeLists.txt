idf_component_register(SRCS "mqtt.c"
                    INCLUDE_DIRS "include"
                    REQUIRES mqtt freertos nvs_flash persistence ota_manager)