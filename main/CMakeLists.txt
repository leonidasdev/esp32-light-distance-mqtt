idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES webserver wifi_manager mqtt_manager persistence adc_manager telegram_manager deepsleep_manager hcsr04_driver
                             esp_event nvs_flash freertos)

fatfs_create_spiflash_image(storage "../filesystem" FLASH_IN_PROJECT)