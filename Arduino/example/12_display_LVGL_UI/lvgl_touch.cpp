#include "lvgl_touch.h"
#include "lvgl_v8_port.h"
#include <lvgl.h>

// 驱动只报原始事件；此层负责单指选择、跨任务同步和 LVGL 输入注册。
struct Sample { lv_point_t point; lv_indev_state_t state; bool cancel; };
static Sample queue[32];
static unsigned head, count;
static int selected_id = -1;
static lv_indev_t *input;
static lv_obj_t *cursor;
static Sample current = {{0, 0}, LV_INDEV_STATE_RELEASED, false};
static bool wait_all_up;
static uint16_t active_ids;

bool lvgl_touch_get_state(uint16_t *x, uint16_t *y, bool *pressed) {
    if (!x || !y || !pressed || !lvgl_port_lock(-1)) return false;
    bool ready = input != nullptr;
    if (ready) {
        *x = current.point.x; *y = current.point.y;
        *pressed = current.state == LV_INDEV_STATE_PRESSED;
    }
    lvgl_port_unlock();
    return ready;
}

// LVGL 后台任务在移植层锁内调用。队列保留快速 DOWN/UP，避免只缓存最终状态漏点。
static void read_pointer(lv_indev_drv_t *, lv_indev_data_t *data) {
    bool cancel = false;
    if (count) {
        current = queue[head];
        head = (head + 1) % 32;
        --count;
        cancel = current.cancel;
        current.cancel = false;
    }
    if (cancel) {
        // 清除正在处理的按压，避免异常 CANCEL/队列溢出被解释成正常 CLICKED。
        lv_indev_reset(input, nullptr);
    }
    data->point = current.point;
    data->state = current.state;
    data->continue_reading = count != 0;
    if (data->state == LV_INDEV_STATE_PRESSED) lv_obj_clear_flag(cursor, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(cursor, LV_OBJ_FLAG_HIDDEN);
}

esp_err_t lvgl_touch_init(uint16_t width, uint16_t height) {
    if (!width || !height) return ESP_ERR_INVALID_ARG;
    if (!lvgl_port_lock(-1)) return ESP_ERR_INVALID_STATE;
    if (input) { lvgl_port_unlock(); return ESP_ERR_INVALID_STATE; }
    lv_disp_t *display = lv_disp_get_default();
    if (!display || width != lv_disp_get_hor_res(display) || height != lv_disp_get_ver_res(display)) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    // 光标放在系统层，不修改彩条/棋盘格绘图对象，也不参与点击命中。
    cursor = lv_obj_create(lv_disp_get_layer_sys(display));
    if (!cursor) { lvgl_port_unlock(); return ESP_ERR_NO_MEM; }
    lv_obj_remove_style_all(cursor);
    lv_obj_set_size(cursor, 18, 18);
    lv_obj_set_style_translate_x(cursor, -9, 0); // 圆点中心对齐实际触摸坐标。
    lv_obj_set_style_translate_y(cursor, -9, 0);
    lv_obj_set_style_radius(cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cursor, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cursor, lv_color_black(), 0);
    lv_obj_set_style_border_width(cursor, 3, 0);
    lv_obj_clear_flag(cursor, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cursor, LV_OBJ_FLAG_HIDDEN);
    static lv_indev_drv_t driver;
    lv_indev_drv_init(&driver);
    driver.type = LV_INDEV_TYPE_POINTER;
    driver.disp = display;
    driver.read_cb = read_pointer;
    input = lv_indev_drv_register(&driver);
    if (!input) {
        lv_obj_del(cursor); cursor = nullptr;
        lvgl_port_unlock(); return ESP_ERR_NO_MEM;
    }
    lv_indev_set_cursor(input, cursor);
    lvgl_port_unlock();
    return ESP_OK;
}

void lvgl_touch_submit(cst3530_event_type_t event, const cst3530_point_t *point) {
    if (!point || point->id >= 16) return;
    if (!lvgl_port_lock(-1)) return;
    if (!input) { lvgl_port_unlock(); return; }
    const uint16_t bit = 1U << point->id;
    if (event == CST3530_DOWN || event == CST3530_MOVE) active_ids |= bit;
    else active_ids &= ~bit;
    if (wait_all_up) {
        if (!active_ids) wait_all_up = false;
        lvgl_port_unlock(); return;
    }
    if (selected_id < 0 && event == CST3530_DOWN) selected_id = point->id;
    if (selected_id != point->id) { lvgl_port_unlock(); return; }
    Sample sample = {{(lv_coord_t)point->x, (lv_coord_t)point->y},
                     point->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED,
                     event == CST3530_CANCEL};
    if (event == CST3530_UP || event == CST3530_CANCEL) {
        sample.state = LV_INDEV_STATE_RELEASED;
        selected_id = -1;
        wait_all_up = active_ids != 0; // 第一根松开后，不跳到仍按住的其他手指。
    }
    // 相邻 MOVE 可合并，但不能覆盖尚未消费的 DOWN/UP 顺序。
    if (event == CST3530_MOVE && count > 1) {
        unsigned last = (head + count - 1) % 32;
        unsigned previous = (head + count - 2) % 32;
        if (queue[last].state == LV_INDEV_STATE_PRESSED &&
            queue[previous].state == LV_INDEV_STATE_PRESSED && !queue[last].cancel) {
            queue[last] = sample;
            lvgl_port_unlock(); return;
        }
    }
    if (count == 32) {
        // 不丢失释放后留下卡住的按压：溢出强制取消并等待全部抬手。
        head = count = 0;
        selected_id = -1;
        wait_all_up = active_ids != 0;
        sample.state = LV_INDEV_STATE_RELEASED;
        sample.cancel = true;
    }
    queue[(head + count) % 32] = sample;
    ++count;
    lvgl_port_unlock();
}
