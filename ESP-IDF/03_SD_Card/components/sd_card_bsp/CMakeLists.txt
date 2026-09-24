idf_component_register(
    SRCS "sd_card_bsp.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_gpio freertos
)