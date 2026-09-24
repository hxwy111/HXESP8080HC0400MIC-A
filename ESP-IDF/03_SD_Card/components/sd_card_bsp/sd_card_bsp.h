#ifndef SD_CARD_BSP_H
#define SD_CARD_BSP_H

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t SD_card_Init(void);
esp_err_t s_example_write_file(const char *path, const char *data);
esp_err_t s_example_read_file(const char *path,
                             char *buffer,
                             size_t buffer_size,
                             size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif