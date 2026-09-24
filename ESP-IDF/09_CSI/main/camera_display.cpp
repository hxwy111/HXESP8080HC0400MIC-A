#include "camera_display.h"
#include "ov5647_camera.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mipi_power.h"
#include "jd9365_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"

static esp_lcd_dsi_bus_handle_t bus;
static esp_lcd_panel_io_handle_t io;
static esp_lcd_panel_handle_t panel;
static void *scan_frames[2];
static unsigned front_index;
static uint32_t refresh_count;
static bool ready, lit, attempted;
static constexpr gpio_num_t BL_EN = GPIO_NUM_33;
static constexpr gpio_num_t BL_PWM = GPIO_NUM_26;
static constexpr int LCD_WIDTH = 720, LCD_HEIGHT = 720;
static constexpr size_t BYTES = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
static constexpr size_t CAMERA_BYTES = OV5647_FRAME_WIDTH * OV5647_FRAME_HEIGHT * sizeof(uint16_t);
static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr), "Refresh counter must be lock-free");
static bool IRAM_ATTR refresh_done(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *) {
    __atomic_fetch_add(&refresh_count, 1U, __ATOMIC_RELAXED);
    return false;
}
static esp_err_t reg(uint8_t cmd, uint8_t val) {
    return esp_lcd_panel_io_tx_param(io, cmd, &val, 1);
}
// Write only the back buffer. Keep the camera frame acquired until scaling completes.
esp_err_t camera_display_show(const void *pixels, size_t size) {
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (!pixels || size != CAMERA_BYTES) return ESP_ERR_INVALID_ARG;
    const unsigned next = front_index ^ 1U;
    auto *dst = static_cast<uint16_t *>(scan_frames[next]);
    const auto *src = static_cast<const uint16_t *>(pixels);
    for (int y = 0; y < LCD_HEIGHT; ++y) {
        const int sy = y * OV5647_FRAME_HEIGHT / LCD_HEIGHT;
        for (int x = 0; x < LCD_WIDTH; ++x)
            dst[y * LCD_WIDTH + x] = src[sy * OV5647_FRAME_WIDTH + x * OV5647_FRAME_WIDTH / LCD_WIDTH];
    }
    vTaskDelay(1);
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, dst);
    if (err == ESP_OK) {
        // Sample AFTER submission. Two refresh events conservatively cover a switch
        // racing with the current DMA completion; never reuse the old buffer early.
        const uint32_t start_count = __atomic_load_n(&refresh_count, __ATOMIC_RELAXED);
        const int64_t deadline = esp_timer_get_time() + 500000;
        while (__atomic_load_n(&refresh_count, __ATOMIC_RELAXED) - start_count < 2U) {
            if (esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
            vTaskDelay(1);
        }
    }
    if (err != ESP_OK) {
        gpio_set_level(BL_EN, 0);
        gpio_set_level(BL_PWM, 1);
        ready = false;
        return err;
    }
    front_index = next;
    if (!lit) {
        err = gpio_set_level(BL_PWM, 0);
        if (err == ESP_OK) err = gpio_set_level(BL_EN, 1);
        lit = err == ESP_OK;
    }
    return err;
}
#define TRY(call) do { esp_err_t e = (call); if(e != ESP_OK) { gpio_set_level(BL_EN, 0); return e; } } while(0)
esp_err_t camera_display_init() {
    if (attempted) return ESP_ERR_INVALID_STATE;
    attempted = true;
    TRY(gpio_set_level(BL_EN, 0));
    TRY(gpio_set_direction(BL_EN, GPIO_MODE_OUTPUT));
    TRY(gpio_set_level(BL_PWM, 1));
    TRY(gpio_set_direction(BL_PWM, GPIO_MODE_OUTPUT));
    vTaskDelay(pdMS_TO_TICKS(200));
    TRY(mipi_power_init());
    esp_lcd_dsi_bus_config_t bc = {};
    bc.bus_id = 0; bc.num_data_lanes = 2;
    bc.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bc.lane_bit_rate_mbps = 500;
    TRY(esp_lcd_new_dsi_bus(&bc, &bus));
    esp_lcd_dbi_io_config_t ic = {};
    ic.virtual_channel = 0; ic.lcd_cmd_bits = 8; ic.lcd_param_bits = 8;
    TRY(esp_lcd_new_panel_io_dbi(bus, &ic, &io));
    TRY(gpio_set_level(GPIO_NUM_27, 1));
    TRY(gpio_set_direction(GPIO_NUM_27, GPIO_MODE_OUTPUT));
    vTaskDelay(pdMS_TO_TICKS(5)); TRY(gpio_set_level(GPIO_NUM_27, 0));
    vTaskDelay(pdMS_TO_TICKS(10)); TRY(gpio_set_level(GPIO_NUM_27, 1)); vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_dpi_panel_config_t dc = {};
    dc.virtual_channel = 0; dc.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dc.dpi_clock_freq_mhz = 30.0f; // 720x720, nominal 49.34 Hz
    dc.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565; dc.num_fbs = 2;
    dc.video_timing.h_size = LCD_WIDTH; dc.video_timing.v_size = LCD_HEIGHT;
    dc.video_timing.hsync_pulse_width = 20;
    dc.video_timing.hsync_back_porch = 20; dc.video_timing.hsync_front_porch = 40;
    dc.video_timing.vsync_pulse_width = 4;
    dc.video_timing.vsync_back_porch = 12; dc.video_timing.vsync_front_porch = 24;
    TRY(esp_lcd_new_panel_dpi(bus, &dc, &panel));
    TRY(esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &scan_frames[0], &scan_frames[1]));
    esp_lcd_dpi_panel_event_callbacks_t callbacks = {};
    callbacks.on_refresh_done = refresh_done;
    TRY(esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, nullptr));
    vTaskDelay(pdMS_TO_TICKS(10));
    for (const auto &entry : PANEL_REGISTERS) TRY(reg(entry.command, entry.value));
    TRY(reg(0xE0, 0)); TRY(reg(0x80, 1)); TRY(reg(0x36, 0)); TRY(reg(0x3A, 0x55));
    TRY(esp_lcd_panel_io_tx_param(io, 0x11, nullptr, 0)); vTaskDelay(pdMS_TO_TICKS(120));
    TRY(esp_lcd_panel_io_tx_param(io, 0x29, nullptr, 0)); vTaskDelay(pdMS_TO_TICKS(5));
    for (void *frame : scan_frames) {
        memset(frame, 0, BYTES);
        TRY(esp_cache_msync(frame, BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA));
    }
    TRY(esp_lcd_panel_init(panel));
    ESP_LOGI("LCD", "720x720 RGB565, 30 MHz, DSI 2 lanes at 500 Mbps; camera 800x800 scaled, double buffered");
    ready = true;
    return ESP_OK;
}


