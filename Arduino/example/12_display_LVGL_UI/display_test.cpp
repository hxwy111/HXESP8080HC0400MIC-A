#include "display_test.h"
// 多页面 UI 演示；保留已验证的屏幕初始化和背光控制。
#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "driver/gpio.h"
#include "lvgl_v8_port.h"
#include "tp_test_ui.h"

// 新硬件：R43 删除，C39 改为 10kΩ 下拉，复位/高阻期间 EN 默认关闭。
static constexpr gpio_num_t BL_EN = GPIO_NUM_33; // 软件主动输出高电平开启背光。
static constexpr gpio_num_t BL_PWM = GPIO_NUM_26; // 保留原调光网络，低电平为已验证的全亮设置。
static bool display_ready = false;

static bool backlight_init() {
  // 先预置输出锁存值，再启用输出，初始化期间保持 EN 低、调光脚高。
  return gpio_set_level(BL_EN, 0) == ESP_OK &&
         gpio_set_direction(BL_EN, GPIO_MODE_OUTPUT) == ESP_OK &&
         gpio_set_level(BL_PWM, 1) == ESP_OK &&
         gpio_set_direction(BL_PWM, GPIO_MODE_OUTPUT) == ESP_OK;
}

static bool backlight_set(bool enable) {
  if (enable) {
    // 首帧准备好后，先设全亮调光电平，再使能升压。
    return gpio_set_level(BL_PWM, 0) == ESP_OK &&
           gpio_set_level(BL_EN, 1) == ESP_OK;
  }
  // 关闭时优先拉低 EN；两次写入都执行，避免其中一次失败跳过另一个引脚。
  const esp_err_t en_result = gpio_set_level(BL_EN, 0);
  const esp_err_t pwm_result = gpio_set_level(BL_PWM, 1);
  return en_result == ESP_OK && pwm_result == ESP_OK;
}

// 保留必要的失败保护，不输出应用层诊断日志。
static bool require_success(bool success) {
  if (success) return true;
  (void)backlight_set(false);
  display_ready = false;
  return false;
}

bool display_test_is_ready() { return display_ready; }

void display_test_begin() {
  Serial.begin(115200);
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
  // LVGL 后台任务负责开机动画、页面和控件；保持主循环及时处理 TP。
  delay(5);
}
