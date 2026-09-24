#include "bsp_i2c.h"
#include "cst3530.h"
#include "driver/gpio.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jd9365_init.h"
#include "lvgl.h"
#include "demos/lv_demos.h"
#include <stdio.h>
#include <string.h>

static constexpr int W = 720, H = 720, DRAW_LINES = 40;
static esp_lcd_dsi_bus_handle_t bus;
static esp_lcd_panel_io_handle_t io;
static esp_lcd_panel_handle_t panel;
static void *lcd_fb;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lv_buf;
static lv_indev_t *touch_indev;
static bool touch_down;
static uint16_t touch_x, touch_y;
static uint16_t touch_width = W, touch_height = H;
// Map the controller's native coordinate range to the actual LCD resolution.
static uint16_t map_touch(uint16_t value, uint16_t source_size, uint16_t target_size) {
  if (source_size <= 1) return 0;
  if (value >= source_size) value = source_size - 1;
  return (uint32_t)value * (target_size - 1) / (source_size - 1);
}

// 修改后的背光控制：EN 为高电平开启，PWM/调光脚为低电平全亮。
static constexpr gpio_num_t BL_EN = GPIO_NUM_33;
static constexpr gpio_num_t BL_PWM = GPIO_NUM_26;

static esp_err_t backlight_init()
{
  // 先保持背光关闭，避免屏幕初始化过程中出现瞬时亮屏。
  esp_err_t err = gpio_set_level(BL_EN, 0);
  if (err == ESP_OK) err = gpio_set_direction(BL_EN, GPIO_MODE_OUTPUT);
  // 新硬件中 GPIO26 为调光控制，低电平表示全亮。
  if (err == ESP_OK) err = gpio_set_level(BL_PWM, 1);
  if (err == ESP_OK) err = gpio_set_direction(BL_PWM, GPIO_MODE_OUTPUT);
  return err;
}

static esp_err_t backlight_set(bool enable)
{
  esp_err_t err = gpio_set_level(BL_PWM, enable ? 0 : 1);
  if (err == ESP_OK) err = gpio_set_level(BL_EN, enable ? 1 : 0);
  return err;
}

static esp_err_t panel_cmd(uint8_t c, uint8_t v) {
  return esp_lcd_panel_io_tx_param(io, c, &v, 1);
}
static void flush_cb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *p) {
  uint16_t *dst = (uint16_t *)lcd_fb;
  const uint16_t *src = (const uint16_t *)p;
  const int width = a->x2 - a->x1 + 1;
  for (int y = a->y1; y <= a->y2; ++y) {
    memcpy(dst + y * W + a->x1, src + (y - a->y1) * width, width * sizeof(uint16_t));
  }
  ESP_ERROR_CHECK(esp_cache_msync(lcd_fb, W * H * sizeof(uint16_t),
                                  ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                      ESP_CACHE_MSYNC_FLAG_TYPE_DATA));
  lv_disp_flush_ready(d);
}
struct TouchSample {
  uint16_t x, y;
  bool down;
};
static TouchSample touch_queue[32];
static unsigned touch_head, touch_count;
static int active_finger = -1;
static esp_timer_handle_t tick_timer;

// Only the LVGL tick is updated from the timer task. UI and touch stay in app_main.
static void tick_cb(void *) {
  lv_tick_inc(2);
}
static void touch_read(lv_indev_drv_t *, lv_indev_data_t *d) {
  if (touch_count) {
    const TouchSample &s = touch_queue[touch_head];
    touch_x = s.x;
    touch_y = s.y;
    touch_down = s.down;
    touch_head = (touch_head + 1) % 32;
    --touch_count;
  }
  d->point.x = touch_x;
  d->point.y = touch_y;
  d->state = touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  d->continue_reading = touch_count != 0;
}
static void on_touch(cst3530_event_type_t e, const cst3530_point_t *p, void *) {
  if (e == CST3530_DOWN && active_finger < 0)
    active_finger = p->id;
  if (active_finger != p->id)
    return;
  if (e == CST3530_CANCEL) {
    touch_head = touch_count = 0;
    touch_down = false;
    active_finger = -1;
    lv_indev_reset(touch_indev, nullptr);
    return;
  }
  const bool down = e == CST3530_DOWN || e == CST3530_MOVE;
  TouchSample sample = {map_touch(p->x, touch_width, W), map_touch(p->y, touch_height, H),
                        down};
  // Coalesce movement, but retain press/release transitions between LVGL reads.
  if (e == CST3530_MOVE && touch_count && touch_queue[(touch_head + touch_count - 1) % 32].down) {
    touch_queue[(touch_head + touch_count - 1) % 32] = sample;
  } else {
    if (touch_count == 32) {
      lv_indev_reset(touch_indev, nullptr);
      touch_head = touch_count = 0;
      touch_down = false;
    }
    touch_queue[(touch_head + touch_count) % 32] = sample;
    ++touch_count;
  }
  if (e != CST3530_MOVE)
    printf("TP event: %s x=%u y=%u\n", down ? "DOWN" : "UP", sample.x, sample.y);
  if (!down)
    active_finger = -1;
}
static void fix_demo_tab_layout(lv_obj_t *parent) {
  uint32_t count = lv_obj_get_child_cnt(parent);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *obj = lv_obj_get_child(parent, (int32_t)i);
    if (lv_obj_check_type(obj, &lv_tabview_class)) {
      lv_obj_t *tabs = lv_tabview_get_tab_btns(obj);
      lv_obj_set_style_pad_left(tabs, 0, 0);
      lv_obj_set_width(tabs, W);
      uint32_t tab_count = lv_obj_get_child_cnt(tabs);
      for (uint32_t n = 0; n < tab_count; ++n) {
        lv_obj_t *tab = lv_obj_get_child(tabs, (int32_t)n);
        lv_obj_set_width(tab, W / 3);
      }
      printf("LVGL: fixed demo tab layout (%u tabs)\n", (unsigned)tab_count);
      return;
    }
    fix_demo_tab_layout(obj);
  }
}
extern "C" void app_main() {
  printf("\nLVGL + CST3530: 720x720 RGB565, 30 MHz, DSI 2 lanes at 500 Mbps\n");
  ESP_ERROR_CHECK(backlight_init());
  vTaskDelay(pdMS_TO_TICKS(200)); // LCD supply settling.
  ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_27, 1));
  ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_27, GPIO_MODE_OUTPUT));
  esp_ldo_channel_config_t ldo = {.chan_id = 3, .voltage_mv = 2500};
  esp_ldo_channel_handle_t ph;
  ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo, &ph));
  vTaskDelay(pdMS_TO_TICKS(20));
  esp_lcd_dsi_bus_config_t bc = {};
  bc.bus_id = 0;
  bc.num_data_lanes = 2;
  bc.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
  bc.lane_bit_rate_mbps = 500;
  ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bc, &bus));
  esp_lcd_dbi_io_config_t ic = {};
  ic.virtual_channel = 0;
  ic.lcd_cmd_bits = 8;
  ic.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus, &ic, &io));
  vTaskDelay(pdMS_TO_TICKS(5) + 1);
  gpio_set_level(GPIO_NUM_27, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(GPIO_NUM_27, 1);
  vTaskDelay(pdMS_TO_TICKS(120));
  esp_lcd_dpi_panel_config_t dc = {};
  dc.virtual_channel = 0;
  dc.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dc.dpi_clock_freq_mhz = 30.0f;
  dc.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
  dc.num_fbs = 1;
  dc.video_timing = {.h_size = W,
                     .v_size = H,
                     .hsync_pulse_width = 20,
                     .hsync_back_porch = 20,
                     .hsync_front_porch = 40,
                     .vsync_pulse_width = 4,
                     .vsync_back_porch = 12,
                     .vsync_front_porch = 24};
  ESP_ERROR_CHECK(esp_lcd_new_panel_dpi(bus, &dc, &panel));
  ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &lcd_fb));
  vTaskDelay(pdMS_TO_TICKS(10));
  for (const auto &r : PANEL_REGISTERS) {
    ESP_ERROR_CHECK(panel_cmd(r.command, r.value));
  }
  ESP_ERROR_CHECK(panel_cmd(0xE0, 0));
  ESP_ERROR_CHECK(panel_cmd(0x80, 1));
  ESP_ERROR_CHECK(panel_cmd(0x36, 0));
  ESP_ERROR_CHECK(panel_cmd(0x3A, 0x55));
  ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x11, nullptr, 0));
  vTaskDelay(pdMS_TO_TICKS(120));
  ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0x29, nullptr, 0));
  vTaskDelay(pdMS_TO_TICKS(5) + 1);
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ESP_ERROR_CHECK(bsp_i2c_init(0, 7, 8, 100000));
  printf("TP: I2C ready\n");
  esp_err_t tp_err = cst3530_init(23, 2);
  printf("TP: reset/probe=%s\n", esp_err_to_name(tp_err));
  cst3530_info_t ti = {.resolution_x = W, .resolution_y = H};
  if (tp_err == ESP_OK) {
    tp_err = cst3530_read_info(&ti);
    printf("TP: info=%s resolution=%ux%u\n", esp_err_to_name(tp_err), ti.resolution_x,
           ti.resolution_y);
    if (!ti.resolution_x || !ti.resolution_y) {
      ti.resolution_x = W;
      ti.resolution_y = H;
    }
  }
  printf("TP: resolution=%ux%u firmware=0x%08lx\n", ti.resolution_x, ti.resolution_y,
         (unsigned long)ti.firmware_version);
  touch_width = ti.resolution_x;
  touch_height = ti.resolution_y;
  lv_init();
  lv_buf = (lv_color_t *)heap_caps_malloc(W * DRAW_LINES * sizeof(lv_color_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
  ESP_ERROR_CHECK(lv_buf ? ESP_OK : ESP_ERR_NO_MEM);
  lv_disp_draw_buf_init(&draw_buf, lv_buf, nullptr, W * DRAW_LINES);
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = W;
  dd.ver_res = H;
  dd.flush_cb = flush_cb;
  dd.draw_buf = &draw_buf;
  lv_disp_drv_register(&dd);
  static lv_indev_drv_t td;
  lv_indev_drv_init(&td);
  td.type = LV_INDEV_TYPE_POINTER;
  td.read_cb = touch_read;
  touch_indev = lv_indev_drv_register(&td);
  (void)touch_indev;
  esp_timer_create_args_t tick_args = {};
  tick_args.callback = tick_cb;
  tick_args.name = "lvgl_tick";
  ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000));

  lv_demo_widgets();
  fix_demo_tab_layout(lv_scr_act());
  lv_refr_now(nullptr);
  if (tp_err == ESP_OK) {
    tp_err = cst3530_start(&ti, on_touch, nullptr);
    printf("TP: start=%s\n", esp_err_to_name(tp_err));
  }
  ESP_ERROR_CHECK(backlight_set(true));
  printf("LVGL: ready; backlight ON\n");
  int64_t last_error_log = 0;
  while (true) {
    if (tp_err == ESP_OK) {
      esp_err_t err = cst3530_poll();
      int64_t now = esp_timer_get_time();
      if (err != ESP_OK && now - last_error_log >= 1000000) {
        printf("TP poll: %s\n", esp_err_to_name(err));
        last_error_log = now;
      }
    }
    lv_timer_handler();
    // One tick is 10 ms at CONFIG_FREERTOS_HZ=100; never busy-loop.
    vTaskDelay(1);
  }
}
