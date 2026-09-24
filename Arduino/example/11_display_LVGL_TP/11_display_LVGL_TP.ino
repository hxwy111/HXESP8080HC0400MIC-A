// 主程序仅编排启动流程；底层 C 模块不依赖 Arduino / LVGL。
#include <Arduino.h>
#include "bsp_i2c.h"
#include "cst3530.h"
#include "display_test.h"
#include "lvgl_touch.h"

static bool tp_ready = false;

// 只打印驱动保存的快照，绝不为诊断再读一次芯片，以免消耗新的报告。
static void print_tp_diagnostics() {
  cst3530_diagnostics_t diag;
  if (cst3530_get_diagnostics(&diag) != ESP_OK) return;
  for (unsigned n = 0; n < diag.attempts; ++n) {
    const cst3530_attempt_diag_t &a = diag.attempt[n];
    Serial.printf("TP-DIAG seq=%lu try=%u result=%s INT(before/read/ack)=%d/%d/%d ack=%s bytes=%u\n",
                  (unsigned long)diag.sequence, n + 1, esp_err_to_name(a.result),
                  a.int_before, a.int_after_read, a.int_after_ack,
                  a.ack_attempted ? esp_err_to_name(a.ack_result) : "not-sent", (unsigned)a.received_size);
    if (a.received_size >= 4) {
      Serial.printf("  type=%02X fingers=%u keys=%u", (unsigned)a.raw[2],
                    (unsigned)(a.raw[3] & 15), (unsigned)(a.raw[3] >> 4));
      if (a.checksum_available)
        Serial.printf(" sum(received/calculated)=%04X/%04X",
                      (unsigned)a.checksum_received, (unsigned)a.checksum_calculated);
      else Serial.print(" sum=unavailable");
      Serial.println();
    }
    Serial.print("  raw:");
    for (unsigned i = 0; i < a.received_size; ++i) Serial.printf(" %02X", (unsigned)a.raw[i]);
    Serial.println();
  }
}

// 回调在主循环任务执行；MOVE 限频打印，DOWN/UP/CANCEL 不丢弃。
static void on_touch(cst3530_event_type_t event, const cst3530_point_t *point, void *user) {
  (void)user;
  lvgl_touch_submit(event, point); // 所有事件先交给 LVGL，串口限频不影响触摸输入。
  static uint32_t last_move_ms[16] = {};
  const uint32_t now = millis();
  if (event == CST3530_MOVE && now - last_move_ms[point->id] < 50) return;
  last_move_ms[point->id] = now;
  const char *name = event == CST3530_DOWN ? "DOWN" : event == CST3530_MOVE ? "MOVE" :
                     event == CST3530_UP ? "UP" : "CANCEL";
  Serial.printf("TP: %s id=%u x=%u y=%u pressure=%u\n", name,
                (unsigned)point->id, (unsigned)point->x, (unsigned)point->y,
                (unsigned)point->pressure);
  // 每秒最多一份正常事件快照，与空闲坏帧对照；不为每次 MOVE 打印原始数据。
  static uint32_t last_diag_ms = 0;
  if ((event == CST3530_DOWN || event == CST3530_UP) && now - last_diag_ms >= 1000) {
    last_diag_ms = now;
    print_tp_diagnostics();
  }
}

void setup() {
  Serial.begin(115200);
  display_test_begin(); // 保留现有显示测试，TP 失败不关闭已正常工作的屏幕。
  if (!display_test_is_ready()) return;

  esp_err_t err = bsp_i2c_init(0, 7, 8, 100000);
  if (err != ESP_OK) {
    Serial.printf("TP: I2C init failed: %s\n", esp_err_to_name(err));
    return;
  }
  err = cst3530_init(23, 2);
  if (err != ESP_OK) {
    Serial.printf("TP: reset/probe 0x58 failed: %s\n", esp_err_to_name(err));
    return;
  }
  Serial.println("TP: reset complete, address 0x58 ACK");

  cst3530_info_t info;
  err = cst3530_read_info(&info);
  if (err != ESP_OK) {
    Serial.printf("TP: runtime info failed: %s (no firmware update attempted)\n",
                  esp_err_to_name(err));
    return;
  }
  Serial.printf("TP: type=0x%08lX, firmware=0x%08lX, project=0x%08lX\n",
                (unsigned long)info.chip_type, (unsigned long)info.firmware_version,
                (unsigned long)info.project_id);
  Serial.printf("TP: resolution=%u x %u, TX=%u, RX=%u, keys=%u\n",
                (unsigned)info.resolution_x, (unsigned)info.resolution_y,
                (unsigned)info.tx_channels, (unsigned)info.rx_channels, (unsigned)info.key_count);
  // 不把运行信息中的类型字段直接等同于 Boot 区 Part Number。
  err = lvgl_touch_init(info.resolution_x, info.resolution_y);
  if (err != ESP_OK) {
    Serial.printf("TP: LVGL input init failed: %s\n", esp_err_to_name(err));
    return;
  }
  err = cst3530_start(&info, on_touch, nullptr);
  if (err != ESP_OK) {
    Serial.printf("TP: reporting start failed: %s\n", esp_err_to_name(err));
    return;
  }
  tp_ready = true;
  Serial.println("TP: LVGL pointer enabled; touch to show cursor (first finger only)");
  Serial.println("TP: diagnostics enabled; INT fields are level samples around each transfer");
}

void loop() {
  display_test_poll();
  if (tp_ready) {
    const esp_err_t err = cst3530_poll();
    // 仅每秒打印一次错误，防止总线异常时刷屏；不停止已正常工作的显示。
    static uint32_t last_error_ms = 0;
    if (err != ESP_OK && millis() - last_error_ms >= 1000) {
      last_error_ms = millis();
      Serial.printf("TP: frame error: %s\n", esp_err_to_name(err));
      print_tp_diagnostics();
    }
  }
}
