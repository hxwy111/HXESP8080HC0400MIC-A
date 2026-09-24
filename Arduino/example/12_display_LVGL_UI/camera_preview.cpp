#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL 3
#include "camera_preview.h"
#include "camera_capture.h"
#include "ov5647_camera.h"
#include "lvgl_v8_port.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

// 只负责 LVGL 显示适配，不创建 DSI、不初始化 I2C。驱动为独立 C 模块。
static lv_obj_t *image_obj, *status_obj, *button_text;
static lv_img_dsc_t descriptor;
static uint16_t *pixels;
static constexpr int PREVIEW_SIZE = 360;
static bool requested, running, fault;
static bool busy; // 受 LVGL 锁保护，从提交请求到首帧/停止完成期间禁用开关。
static int64_t last_log_us;
static int64_t last_frame_us, last_update_us;
static uint32_t shown;
static_assert(LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0,
              "Camera preview requires native RGB565");

void camera_preview_bind(lv_obj_t *image, lv_obj_t *status, lv_obj_t *text) {
    esp_log_level_set("CAM_UI", ESP_LOG_INFO);
    image_obj = image; status_obj = status; button_text = text;
}
void camera_preview_request(bool enable) {
    if (fault || (busy && enable)) return;
    requested = enable;
    busy = true;
    lv_obj_add_state(lv_obj_get_parent(button_text), LV_STATE_DISABLED);
    lv_label_set_text(button_text, enable ? "STARTING..." : "STOPPING...");
}
bool camera_preview_busy() { return busy; }
bool camera_preview_requested() { return requested; }

// 所有 UI 写入都持有移植层锁；硬件 start/stop 不在 LVGL 锁内执行。
static void show_status(const char *text) {
    if (!lvgl_port_lock(-1)) return;
    lv_label_set_text(status_obj, text);
    lv_label_set_text(button_text, running ? "CAMERA OFF" : "CAMERA ON");
    lvgl_port_unlock();
}
static void fail(esp_err_t err, const char *where) {
    ESP_LOGE("CAM_UI", "failure=%s error=%s", where, esp_err_to_name(err));
    camera_capture_log(); // 必须先记录，再停止；停止会改变缓冲与阶段。
    esp_err_t stopped = camera_capture_stop();
    running = false;
    if (!lvgl_port_lock(-1)) return;
    fault = true; requested = false;
    busy = false;
    lv_obj_add_state(lv_obj_get_parent(button_text), LV_STATE_DISABLED);
    lv_obj_add_flag(image_obj, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(status_obj, "%s: %s\nstop: %s / reset to retry",
                         where, esp_err_to_name(err), esp_err_to_name(stopped));
    lv_label_set_text(button_text, "RESET REQUIRED");
    lvgl_port_unlock();
}
void camera_preview_poll() {
    if (!lvgl_port_lock(-1)) return;
    bool bound = image_obj != nullptr;
    bool want = requested;
    bool blocked = fault;
    lvgl_port_unlock();
    if (!bound || blocked) return;
    if (want != running) {
        ESP_LOGI("CAM_UI", "request %s", want ? "ON" : "OFF");
        if (want) {
            show_status("Starting camera...");
            if (!pixels) {
                pixels = static_cast<uint16_t *>(heap_caps_malloc(PREVIEW_SIZE * PREVIEW_SIZE * 2, MALLOC_CAP_SPIRAM));
                if (!pixels) { fail(ESP_ERR_NO_MEM, "preview memory"); return; }
                descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
                descriptor.header.w = descriptor.header.h = PREVIEW_SIZE;
                descriptor.data_size = PREVIEW_SIZE * PREVIEW_SIZE * 2;
                descriptor.data = reinterpret_cast<const uint8_t *>(pixels);
            }
            esp_err_t err = camera_capture_start();
            if (err != ESP_OK) { fail(err, "capture start"); return; }
            running = true; shown = 0;
            last_frame_us = esp_timer_get_time();
            last_update_us = 0;
            show_status("Waiting for frames...");
        } else {
            esp_err_t err = camera_capture_stop();
            if (err != ESP_OK) { fail(err, "capture stop"); return; }
            running = false;
            if (lvgl_port_lock(-1)) {
                lv_obj_add_flag(image_obj, LV_OBJ_FLAG_HIDDEN);
                busy = false;
                lv_obj_clear_state(lv_obj_get_parent(button_text), LV_STATE_DISABLED);
                lvgl_port_unlock();
            }
            show_status("Camera OFF (standby)");
        }
    }
    // 开始请求被 HOME 取消，且采集尚未启动时，恢复关闭状态。
    if (!running && !want && lvgl_port_lock(-1)) {
        busy = false;
        lv_obj_clear_state(lv_obj_get_parent(button_text), LV_STATE_DISABLED);
        lv_label_set_text(button_text, "CAMERA ON");
        lvgl_port_unlock();
    }
    if (!running) return;
    int64_t now = esp_timer_get_time();
    if (now - last_log_us >= 1000000) {
        camera_capture_log();
        last_log_us = now;
    }
    if (now - last_update_us < 66667) return; // UI 限制约 15fps，CSI 保持原采集时序。
    last_update_us = now;
    const void *frame = nullptr;
    size_t size = 0;
    esp_err_t err = camera_capture_acquire(&frame, &size);
    if (err == ESP_ERR_NOT_FOUND) {
        if (now - last_frame_us > 5000000) fail(ESP_ERR_TIMEOUT, "no valid frame 5s");
        return;
    }
    if (err != ESP_OK) { fail(err, "frame acquire"); return; }
    if (size != OV5647_FRAME_WIDTH * OV5647_FRAME_HEIGHT * 2U) {
        camera_capture_release(frame); fail(ESP_ERR_INVALID_SIZE, "frame size"); return;
    }
    // 相机帧受 acquire/release 保护，独立显示副本由 LVGL 锁保护。
    // 最近邻 800:360 缩小保留全部取景范围，给按钮留出空间。
    if (lvgl_port_lock(-1)) {
        if (!shown) {
            ESP_LOGI("CAM_UI", "first frame received");
            busy = false;
            lv_obj_clear_state(lv_obj_get_parent(button_text), LV_STATE_DISABLED);
            lv_label_set_text(button_text, "CAMERA OFF");
        }
        const uint16_t *source = static_cast<const uint16_t *>(frame);
        for (int y = 0; y < PREVIEW_SIZE; ++y)
            for (int x = 0; x < PREVIEW_SIZE; ++x)
                pixels[y * PREVIEW_SIZE + x] = source[
                    (y * OV5647_FRAME_HEIGHT / PREVIEW_SIZE) * OV5647_FRAME_WIDTH +
                    x * OV5647_FRAME_WIDTH / PREVIEW_SIZE];
        lv_img_cache_invalidate_src(&descriptor);
        lv_img_set_src(image_obj, &descriptor);
        lv_obj_clear_flag(image_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(image_obj);
        lv_label_set_text_fmt(status_obj, "LIVE 360 x 360 / frames %lu", (unsigned long)++shown);
        lvgl_port_unlock();
    }
    camera_capture_release(frame);
    last_frame_us = now;
}
