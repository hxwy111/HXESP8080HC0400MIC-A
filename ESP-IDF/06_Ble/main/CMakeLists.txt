set(srcs "main.c"
         "gatt_svr.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash nimble_peripheral_utils esp_hosted)
