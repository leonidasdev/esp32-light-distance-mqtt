idf_component_register(SRCS "persistence.c"
                    INCLUDE_DIRS "include"
                    REQUIRES fatfs nvs_flash freertos vfs)