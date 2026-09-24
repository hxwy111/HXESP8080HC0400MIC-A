#pragma once
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t camera_display_init(void);
esp_err_t camera_display_show(const void *pixels, size_t size);

#ifdef __cplusplus
}
#endif
