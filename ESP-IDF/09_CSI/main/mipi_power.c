#include "mipi_power.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static esp_ldo_channel_handle_t power;
esp_err_t mipi_power_init(void) {
    if (power) return ESP_OK;
    esp_ldo_channel_config_t config = {.chan_id = 3, .voltage_mv = 2500};
    esp_err_t err = esp_ldo_acquire_channel(&config, &power);
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(20) + 1);
    return err;
}
