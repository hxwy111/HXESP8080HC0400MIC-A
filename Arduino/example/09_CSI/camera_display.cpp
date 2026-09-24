#include "camera_display.h"
#include "ov5647_camera.h"
#include "mipi_power.h"
#include "jd9365_init.h"
#include <Arduino.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"

static esp_lcd_dsi_bus_handle_t bus;
static esp_lcd_panel_io_handle_t io;
static esp_lcd_panel_handle_t panel;
// 独立的缩放暂存区；DSI 驱动内部另分配两个扫描帧缓冲。
static uint16_t *lcd_frame;
static uint16_t *scan_frames[2];
static unsigned current_scan_frame;
static bool ready, lit, attempted;
static constexpr int LCD_WIDTH = 720;
static constexpr int LCD_HEIGHT = 720;
static constexpr int CAMERA_WIDTH = OV5647_FRAME_WIDTH;
static constexpr int CAMERA_HEIGHT = OV5647_FRAME_HEIGHT;
static constexpr size_t LCD_BYTES = LCD_WIDTH * LCD_HEIGHT * 2;
static constexpr size_t CAMERA_BYTES = CAMERA_WIDTH * CAMERA_HEIGHT * 2;
// 新硬件背光：R43 已删除、C39 改为 10kΩ 下拉；GPIO33 必须由软件主动拉高开启。
static constexpr gpio_num_t BL_EN = GPIO_NUM_33;
// GPIO26 为调光输入，本板固定低电平为全亮，高电平关闭调光输出。
static constexpr gpio_num_t BL_PWM = GPIO_NUM_26;
static esp_err_t reg(uint8_t cmd, uint8_t val) {
    return esp_lcd_panel_io_tx_param(io, cmd, &val, 1);
}
// 相机源缓冲由 acquire/release 保护；缩放暂存区与 DSI 扫描缓冲不混用。
esp_err_t camera_display_show(const void *pixels, size_t size) {
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (!pixels || size != CAMERA_BYTES) return ESP_ERR_INVALID_ARG;
    // CSI 仍输出 800x800；显示屏为 720x720，使用最近邻缩放，避免
    // 把 1,280,000 字节直接 memcpy 到 1,036,800 字节的 LCD 缓冲区。
    const uint16_t *src = static_cast<const uint16_t *>(pixels);
    uint16_t *dst = lcd_frame;
    for (int y = 0; y < LCD_HEIGHT; ++y) {
        const int sy = y * CAMERA_HEIGHT / LCD_HEIGHT;
        for (int x = 0; x < LCD_WIDTH; ++x) {
            const int sx = x * CAMERA_WIDTH / LCD_WIDTH;
            dst[y * LCD_WIDTH + x] = src[sy * CAMERA_WIDTH + sx];
        }
    }
    esp_err_t err = esp_cache_msync(lcd_frame, LCD_BYTES,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK) { gpio_set_level(BL_EN, 0); ready = false; return err; }
    if (!lit) {
        delay(20); // 先让首帧进入扫描再开启背光。
        err = gpio_set_level(BL_PWM, 0);
        if (err == ESP_OK) err = gpio_set_level(BL_EN, 1);
        lit = err == ESP_OK;
    }
    if (err == ESP_OK) {
        // 当前扫描帧保持不动，完整写入另一帧后再切换，避免半帧撕裂。
        const unsigned next = current_scan_frame ^ 1U;
        memcpy(scan_frames[next], lcd_frame, LCD_BYTES);
        err = esp_cache_msync(scan_frames[next], LCD_BYTES,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        if (err == ESP_OK) {
            // 传入驱动自己的帧缓冲地址，驱动只需切换索引，不再复制到当前扫描帧。
            err = esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_WIDTH, LCD_HEIGHT,
                                            scan_frames[next]);
            if (err == ESP_OK) current_scan_frame = next;
        }
    }
    return err;
}
#define TRY(call) do { esp_err_t e = (call); if(e != ESP_OK) { gpio_set_level(BL_EN, 0); return e; } } while(0)
esp_err_t camera_display_init() {
    if (attempted) return ESP_ERR_INVALID_STATE;
    attempted = true;
    // 先设定低电平再配置输出：外部 10kΩ 下拉保证复位/高阻期间默认关闭。
    TRY(gpio_set_level(BL_EN, 0));
    TRY(gpio_set_direction(BL_EN, GPIO_MODE_OUTPUT));
    TRY(gpio_set_level(BL_PWM, 1));
    TRY(gpio_set_direction(BL_PWM, GPIO_MODE_OUTPUT));
    delay(200);
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
    delay(5); TRY(gpio_set_level(GPIO_NUM_27, 0));
    delay(10); TRY(gpio_set_level(GPIO_NUM_27, 1)); delay(120);
    esp_lcd_dpi_panel_config_t dc = {};
    dc.virtual_channel = 0; dc.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    // 使用已验证可工作的 30 MHz；36.48 MHz 在此板上曾导致帧缓冲固定青色。
    dc.dpi_clock_freq_mhz = 30.0f;
    dc.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    // 两个内部扫描缓冲：驱动将完整暂存帧复制到空闲缓冲后再切换，避免撕裂。
    dc.num_fbs = 2;
    dc.video_timing.h_size = LCD_WIDTH; dc.video_timing.v_size = LCD_HEIGHT;
    dc.video_timing.hsync_pulse_width = 20;
    dc.video_timing.hsync_back_porch = 20; dc.video_timing.hsync_front_porch = 40;
    dc.video_timing.vsync_pulse_width = 4;
    dc.video_timing.vsync_back_porch = 12; dc.video_timing.vsync_front_porch = 24;
    TRY(esp_lcd_new_panel_dpi(bus, &dc, &panel));
    void *fb0 = nullptr;
    void *fb1 = nullptr;
    TRY(esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1));
    scan_frames[0] = static_cast<uint16_t *>(fb0);
    scan_frames[1] = static_cast<uint16_t *>(fb1);
    current_scan_frame = 0;
    lcd_frame = static_cast<uint16_t *>(heap_caps_calloc(
        LCD_WIDTH * LCD_HEIGHT, sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
    if (!lcd_frame) { gpio_set_level(BL_EN, 0); return ESP_ERR_NO_MEM; }
    delay(10);
    for (const auto &entry : PANEL_REGISTERS) TRY(reg(entry.command, entry.value));
    TRY(reg(0xE0, 0)); TRY(reg(0x80, 1)); TRY(reg(0x36, 0)); TRY(reg(0x3A, 0x55));
    TRY(esp_lcd_panel_io_tx_param(io, 0x11, nullptr, 0)); delay(120);
    TRY(esp_lcd_panel_io_tx_param(io, 0x29, nullptr, 0)); delay(5);
    memset(lcd_frame, 0, LCD_BYTES);
    TRY(esp_cache_msync(lcd_frame, LCD_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA));
    TRY(esp_lcd_panel_init(panel));
    ready = true;
    return ESP_OK;
}
