idf_component_register(SRCS "oled.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_lcd_sh1107 esp_lvgl_port lvgl freertos)