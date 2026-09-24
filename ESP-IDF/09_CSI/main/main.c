/*
 * ESP-IDF 相机预览入口。
 *
 * 启动顺序：
 * I2C -> JD9365 显示/背光 -> OV5647/CSI/ISP -> 帧 acquire/show/release。
 * 不依赖 Arduino 的 camera_bringup 层；所有控制接口在本任务串行调用。
 */
#include "bsp_i2c.h"
#include "camera_capture.h"
#include "camera_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP";

void app_main(void)
{
    esp_err_t err = bsp_i2c_init(0, 7, 8, 100000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return;
    }

    err = camera_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return;
    }

    err = camera_capture_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capture start failed at %s: %s",
                 camera_capture_stage(), esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "preview started; waiting for RGB565 frames");

    uint32_t shown = 0;
    int64_t last_stats = esp_timer_get_time();
    camera_capture_stats_t stats = {0};
    while (true) {
        const void *frame = NULL;
        size_t size = 0;
        err = camera_capture_acquire(&frame, &size);
        if (err == ESP_OK) {
            err = camera_display_show(frame, size);
            camera_capture_release(frame);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "display frame failed: %s", esp_err_to_name(err));
                (void)camera_capture_stop();
                break;
            }
            ++shown;
        } else if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "acquire frame failed: %s", esp_err_to_name(err));
        }

        const int64_t now = esp_timer_get_time();
        if (now - last_stats >= 1000000) {
            camera_capture_stats(&stats);
            ESP_LOGI(TAG, "frames=%lu shown=%lu bytes=%lu bad=%lu",
                     (unsigned long)stats.frames, (unsigned long)shown,
                     (unsigned long)stats.last_bytes,
                     (unsigned long)stats.wrong_size);
            last_stats = now;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
