#include "display_test.h"
// 专用 TP 测试界面；保留已验证的屏幕初始化和背光控制。
#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "driver/gpio.h"
#include "lvgl_v8_port.h"
#include "tp_test_ui.h"

// 新硬件背光：R43 已删除、C39 改为 10kΩ 下拉；GPIO33 必须由软件主动控制。
static constexpr gpio_num_t BL_EN = GPIO_NUM_33;  // AP3032 CTRL/EN，高电平开启
static constexpr gpio_num_t BL_PWM = GPIO_NUM_26; // 调光输入，低电平为本板全亮
static bool display_ready = false;

// 外部 10kΩ 下拉保证复位/高阻期间默认关闭；软件初始化时再次明确设为关闭。
static bool backlight_init() {
  return gpio_set_direction(BL_EN, GPIO_MODE_OUTPUT) == ESP_OK &&
         gpio_set_level(BL_EN, 0) == ESP_OK &&
         gpio_set_direction(BL_PWM, GPIO_MODE_OUTPUT) == ESP_OK &&
         gpio_set_level(BL_PWM, 1) == ESP_OK;
}

// 先设置调光脚，再切换 EN，避免开启瞬间出现不确定亮度。
static bool backlight_set(bool enable) {
  return gpio_set_level(BL_PWM, enable ? 0 : 1) == ESP_OK &&
         gpio_set_level(BL_EN, enable ? 1 : 0) == ESP_OK;
}

// 保留必要的失败保护，不输出应用层诊断日志。
static bool require_success(bool success) {
  if (success) return true;
  backlight_set(false);
  display_ready = false;
  return false;
}

bool display_test_is_ready() { return display_ready; }

void display_test_begin() {
  Serial.begin(115200);
  // 初始化期间关闭背光；GPIO26 高电平关闭调光输出。
  if (!require_success(backlight_init())) return;

  // Board 负责 PHY 供电、DSI、复位及 JD9365 初始化。
  static esp_panel::board::Board board;
  if (!require_success(board.init())) return;
  if (!require_success(board.begin())) return;
  auto lcd = board.getLCD();
  auto backlight = board.getBacklight();
  if (!require_success(lcd && backlight)) return;

  // 移植层负责绘图缓冲、刷新回调、2 ms tick、后台任务及互斥锁。
  // nullptr 表示不注册触摸输入设备。
  if (!require_success(lvgl_port_init(lcd, nullptr))) return;

  if (!require_success(lvgl_port_lock(-1))) return;
  const bool ui_ok = tp_test_ui_create();
  if (ui_ok) lv_refr_now(nullptr);
  lvgl_port_unlock();
  if (!require_success(ui_ok)) return;
  delay(100);
  if (!require_success(backlight_set(true))) return;
  display_ready = true;
}

void display_test_poll() {
  if (!display_ready) { delay(100); return; }
  if (Serial.available()) {
    const char c = Serial.read();
    if (c == 'd' || c == 'e') {
      if (!require_success(backlight_set(c == 'e'))) return;
    } else if (c == 'r') {
      if (!require_success(lvgl_port_lock(-1))) return;
      tp_test_ui_reset();
      lvgl_port_unlock();
    }
  }
  // 无彩条轮播，LVGL 后台任务负责 UI；保持主循环及时处理 TP。
  delay(5);
}
