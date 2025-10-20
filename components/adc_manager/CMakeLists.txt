idf_component_register(SRCS "adc_manager.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_adc freertos)