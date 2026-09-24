#pragma once
#include <stddef.h>
#include "esp_err.h"
esp_err_t camera_display_init();
esp_err_t camera_display_show(const void *pixels, size_t size);
