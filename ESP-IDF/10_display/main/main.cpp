#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jd9365_init.h"

static constexpr int W = 720, H = 720;
static esp_lcd_dsi_bus_handle_t bus;
static esp_lcd_panel_io_handle_t io;
static esp_lcd_panel_handle_t panel;
static esp_ldo_channel_handle_t phy_power;

// 新硬件背光：R43 删除，C39 改为 10kΩ 下拉。
static constexpr gpio_num_t BL_EN = GPIO_NUM_33;
static constexpr gpio_num_t BL_PWM = GPIO_NUM_26;

static void fail(const char *step, esp_err_t err) {
    printf("DISPLAY: %s failed: %s\n", step, esp_err_to_name(err));
    gpio_set_level(BL_EN, 0);
    gpio_set_level(BL_PWM, 1);
}

static esp_err_t command(uint8_t reg, uint8_t value) {
    return esp_lcd_panel_io_tx_param(io, reg, &value, 1);
}

static esp_err_t draw_pattern(void *fb, int pattern) {
    uint16_t *pixels = static_cast<uint16_t *>(fb);
    static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
    static const uint16_t bars[] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0,
                                    0xF81F, 0xF800, 0x001F, 0x0000};
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint16_t color;
            if (pattern < 5) {
                color = colors[pattern];
            } else if (pattern == 5) {
                color = bars[x * 8 / W];
            } else {
                color = ((x / 40 + y / 40) & 1) ? 0xFFFF : 0x0000;
                if (x == W / 2 || y == H / 2) color = 0xF800;
            }
            pixels[y * W + x] = color;
        }
    }
    return esp_cache_msync(fb, W * H * sizeof(uint16_t),
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

extern "C" void app_main(void) {
    printf("\nJD9365 display test: 720x720 RGB565 MIPI-DSI, 30 MHz, 2 lanes at 500 Mbps\n");
    ESP_ERROR_CHECK(gpio_set_level(BL_EN, 0));
    ESP_ERROR_CHECK(gpio_set_direction(BL_EN, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(BL_PWM, 1));
    ESP_ERROR_CHECK(gpio_set_direction(BL_PWM, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_27, 1));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_27, GPIO_MODE_OUTPUT));

    vTaskDelay(pdMS_TO_TICKS(200)); // LCD supply settling, backlight remains off.
    esp_ldo_channel_config_t power = {.chan_id = 3, .voltage_mv = 2500};
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&power, &phy_power));
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_lcd_dsi_bus_config_t bus_cfg = {};
    bus_cfg.bus_id = 0; bus_cfg.num_data_lanes = 2;
    bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_cfg.lane_bit_rate_mbps = 500;
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &bus));
    esp_lcd_dbi_io_config_t io_cfg = {};
    io_cfg.virtual_channel = 0; io_cfg.lcd_cmd_bits = 8; io_cfg.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus, &io_cfg, &io));

    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_27, 0));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_27, 1));
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_lcd_dpi_panel_config_t dpi = {};
    dpi.virtual_channel = 0; dpi.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi.dpi_clock_freq_mhz = 30.0f; dpi.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    dpi.num_fbs = 1;
    dpi.video_timing = {.h_size = W, .v_size = H, .hsync_pulse_width = 20,
        .hsync_back_porch = 20, .hsync_front_porch = 40, .vsync_pulse_width = 4,
        .vsync_back_porch = 12, .vsync_front_porch = 24};
    ESP_ERROR_CHECK(esp_lcd_new_panel_dpi(bus, &dpi, &panel));
    void *fb = nullptr;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb));
    printf("DISPLAY: PSRAM free=%lu\n", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_ERROR_CHECK(draw_pattern(fb, 0));
    vTaskDelay(pdMS_TO_TICKS(10));
    for (const auto &r : PANEL_REGISTERS) ESP_ERROR_CHECK(command(r.command, r.value));
    ESP_ERROR_CHECK(command(0xE0, 0x00));
    ESP_ERROR_CHECK(command(0x80, 0x01));
    ESP_ERROR_CHECK(command(0x36, 0x00));
    ESP_ERROR_CHECK(command(0x3A, 0x55));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x11, nullptr, 0));
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x29, nullptr, 0));
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(gpio_set_level(BL_PWM, 0));
    ESP_ERROR_CHECK(gpio_set_level(BL_EN, 1));
    printf("DISPLAY: backlight ON, color test active (7 patterns, 2 s each)\n");
    int pattern = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        pattern = (pattern + 1) % 7;
        ESP_ERROR_CHECK(draw_pattern(fb, pattern));
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, W, H, fb));
    }
}


