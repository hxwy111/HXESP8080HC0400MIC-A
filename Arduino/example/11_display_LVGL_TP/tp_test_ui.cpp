#include "tp_test_ui.h"
#include "lvgl_touch.h"
#include <lvgl.h>
#include <stdint.h>

static lv_obj_t *targets[5], *summary, *position_label, *state_label;
static lv_obj_t *slider, *slider_label, *hold_label;
static uint32_t clicks, holds;
static uint8_t visited;
static const char *names[] = {"TOP", "LEFT", "CENTER", "RIGHT", "BOTTOM"};
static const uint32_t BG = 0x101923, PANEL = 0x203244, CYAN = 0x39C9DF, GREEN = 0x42D69A;

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w,
                        const lv_font_t *font = &lv_font_montserrat_20) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, w);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xE8F2FA), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w, int h) {
    lv_obj_t *obj = lv_btn_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 22, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(PANEL), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x16667A), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, lv_color_hex(CYAN), 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *text_obj = label(obj, text, 0, 0, w);
    lv_obj_center(text_obj);
    return obj;
}

static void update_summary() {
    unsigned hit = 0;
    for (unsigned i = 0; i < 5; ++i) if (visited & (1U << i)) ++hit;
    lv_label_set_text_fmt(summary, "CLICKS %lu    TARGETS %u / 5", (unsigned long)clicks, hit);
}
static void target_event(lv_event_t *event) {
    const unsigned index = (uintptr_t)lv_event_get_user_data(event);
    ++clicks;
    visited |= 1U << index;
    lv_obj_set_style_bg_color(targets[index], lv_color_hex(0x195348), 0);
    lv_obj_set_style_border_color(targets[index], lv_color_hex(GREEN), 0);
    update_summary();
}
static void slider_event(lv_event_t *) {
    lv_label_set_text_fmt(slider_label, "DRAG  %ld / 100", (long)lv_slider_get_value(slider));
}
static void hold_event(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) {
        ++holds;
        lv_label_set_text_fmt(hold_label, "HOLD OK  %lu", (unsigned long)holds);
    }
}
void tp_test_ui_reset() {
    clicks = holds = 0;
    visited = 0;
    for (auto target : targets) {
        lv_obj_set_style_bg_color(target, lv_color_hex(PANEL), 0);
        lv_obj_set_style_border_color(target, lv_color_hex(CYAN), 0);
    }
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    slider_event(nullptr);
    lv_label_set_text(hold_label, "PRESS AND HOLD");
    update_summary();
}
static void reset_event(lv_event_t *) { tp_test_ui_reset(); }

// 读取适配层已经提交给 LVGL 的指针状态，不额外读 TP，也不跟随第二根手指。
static void status_tick(lv_timer_t *) {
    uint16_t x, y;
    bool pressed;
    if (!lvgl_touch_get_state(&x, &y, &pressed)) {
        lv_label_set_text(state_label, "WAITING FOR TP");
        return;
    }
    static int old_x = -1, old_y = -1, old_pressed = -1;
    if (old_x != x || old_y != y) {
        lv_label_set_text_fmt(position_label, "X %03u   Y %03u", (unsigned)x, (unsigned)y);
        old_x = x; old_y = y;
    }
    if (old_pressed != (int)pressed) {
        lv_label_set_text(state_label, pressed ? "TOUCH DOWN" : "RELEASED");
        lv_obj_set_style_text_color(state_label, lv_color_hex(pressed ? GREEN : CYAN), 0);
        old_pressed = pressed;
    }
}

bool tp_test_ui_create() {
    lv_obj_t *screen = lv_scr_act();
    if (lv_disp_get_hor_res(nullptr) != 720 || lv_disp_get_ver_res(nullptr) != 720) return false;
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_20, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    // 布局集中在圆屏可视区域；不依赖中文字库，避免中文显示成方框。
    // 图一的 800x800 布局按 0.9 缩放到 720x720，确保底部 RESET 和右侧按钮完整可见。
    label(screen, "TOUCH LAB", 180, 50, 360, &lv_font_montserrat_32);
    label(screen, "CST3530  /  720 x 720", 180, 83, 360, &lv_font_montserrat_16);
    targets[0] = button(screen, names[0], 306, 117, 108, 68);
    label(screen, "Tap all 5 targets - green means hit", 117, 203, 486, &lv_font_montserrat_16);
    position_label = label(screen, "X ---   Y ---", 189, 240, 342, &lv_font_montserrat_32);
    state_label = label(screen, "WAITING FOR TP", 198, 281, 324);
    summary = label(screen, "", 171, 315, 378, &lv_font_montserrat_16);
    targets[1] = button(screen, names[1], 59, 333, 108, 72);
    targets[2] = button(screen, names[2], 306, 342, 108, 72);
    targets[3] = button(screen, names[3], 554, 333, 108, 72);
    targets[4] = button(screen, names[4], 306, 577, 108, 68);
    for (unsigned i = 0; i < 5; ++i)
        lv_obj_add_event_cb(targets[i], target_event, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

    slider_label = label(screen, "DRAG  50 / 100", 216, 427, 288);
    slider = lv_slider_create(screen);
    lv_obj_set_pos(slider, 198, 469);
    lv_obj_set_size(slider, 324, 16);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(GREEN), LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 18);
    lv_obj_add_event_cb(slider, slider_event, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *hold = button(screen, "PRESS AND HOLD", 216, 513, 288, 50);
    hold_label = lv_obj_get_child(hold, 0);
    lv_obj_add_event_cb(hold, hold_event, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_t *reset = button(screen, "RESET", 297, 661, 126, 36);
    lv_obj_add_event_cb(reset, reset_event, LV_EVENT_CLICKED, nullptr);
    tp_test_ui_reset();
    return lv_timer_create(status_tick, 50, nullptr) != nullptr;
}
