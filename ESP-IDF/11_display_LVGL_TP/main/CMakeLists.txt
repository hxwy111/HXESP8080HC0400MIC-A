idf_component_register(SRCS "main.cpp" "bsp_i2c.c" "cst3530.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_driver_cam esp_lcd esp_mm esp_timer lvgl)
