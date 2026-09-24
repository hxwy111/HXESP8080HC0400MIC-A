#include "tp_test_ui.h"
#include "lvgl_touch.h"
#include "camera_preview.h"
#include <lvgl.h>
#include <stdint.h>

static lv_obj_t *targets[5], *summary, *position_label, *state_label;
static lv_obj_t *slider, *slider_label, *hold_label;
static uint32_t clicks, holds;
static uint8_t visited;
static const char *names[] = {"TOP", "LEFT", "CENTER", "RIGHT", "BOTTOM"};
static const uint32_t BG = 0x101923, PANEL = 0x203244, CYAN = 0x39C9DF, GREEN = 0x42D69A;
// 所有页面与 UI 回调集中在本文件；页面只创建一次，切换时保留控件状态。
static lv_obj_t *home_screen, *touch_screen, *widget_screen, *test_screen, *boot_screen;
static lv_obj_t *test_content;
static unsigned pattern;
static bool test_long_press;
static lv_obj_t *camera_screen;

// Preserve the original 800px design proportions on the 720px panel.
static int ui_px(int value) { return (value * 9 + 5) / 10; }

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w,
                        const lv_font_t *font = &lv_font_montserrat_18) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, ui_px(x), ui_px(y));
    lv_obj_set_width(obj, ui_px(w));
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xE8F2FA), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w, int h) {
    lv_obj_t *obj = lv_btn_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, ui_px(x), ui_px(y));
    lv_obj_set_size(obj, ui_px(w), ui_px(h));
    lv_obj_set_style_radius(obj, ui_px(22), 0);
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
    if (lv_scr_act() != touch_screen) return; // 隐藏页面不刷新坐标文本。
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

static bool create_touch_page(lv_obj_t *screen) {
    if (lv_disp_get_hor_res(nullptr) != 720 || lv_disp_get_ver_res(nullptr) != 720) return false;
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_18, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    // 布局集中在圆屏可视区域；不依赖中文字库，避免中文显示成方框。
    label(screen, "TOUCH LAB", 200, 55, 400, &lv_font_montserrat_28);
    label(screen, "CST3530  /  720 x 720", 200, 92, 400, &lv_font_montserrat_14);
    targets[0] = button(screen, names[0], 340, 130, 120, 76);
    label(screen, "Tap all 5 targets - green means hit", 130, 226, 540, &lv_font_montserrat_14);
    position_label = label(screen, "X ---   Y ---", 210, 267, 380, &lv_font_montserrat_28);
    state_label = label(screen, "WAITING FOR TP", 220, 312, 360);
    summary = label(screen, "", 190, 350, 420, &lv_font_montserrat_14);
    targets[1] = button(screen, names[1], 65, 370, 120, 80);
    targets[2] = button(screen, names[2], 340, 380, 120, 80);
    targets[3] = button(screen, names[3], 615, 370, 120, 80);
    targets[4] = button(screen, names[4], 340, 641, 120, 76);
    for (unsigned i = 0; i < 5; ++i)
        lv_obj_add_event_cb(targets[i], target_event, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

    slider_label = label(screen, "DRAG  50 / 100", 240, 474, 320);
    slider = lv_slider_create(screen);
    lv_obj_set_pos(slider, ui_px(220), ui_px(521));
    lv_obj_set_size(slider, ui_px(360), ui_px(18));
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(GREEN), LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 18);
    lv_obj_add_event_cb(slider, slider_event, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *hold = button(screen, "PRESS AND HOLD", 220, 570, 270, 55);
    hold_label = lv_obj_get_child(hold, 0);
    lv_obj_add_event_cb(hold, hold_event, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_t *reset = button(screen, "RESET", 510, 570, 130, 55);
    lv_obj_add_event_cb(reset, reset_event, LV_EVENT_CLICKED, nullptr);
    tp_test_ui_reset();
    return lv_timer_create(status_tick, 50, nullptr) != nullptr;
}

// ---------- 通用页面和导航 ----------
static lv_obj_t *new_screen() {
    lv_obj_t *s = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, ui_px(800), ui_px(800));
    lv_obj_set_style_bg_color(s, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_18, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}
static void go_home(lv_event_t *) {
    if (lv_scr_act() == camera_screen) camera_preview_request(false);
    lv_scr_load(home_screen);
}
static void back_button(lv_obj_t *s) {
    lv_obj_t *b = button(s, LV_SYMBOL_HOME "  HOME", 300, 724, 200, 48);
    lv_obj_add_event_cb(b, go_home, LV_EVENT_CLICKED, nullptr);
}
static void open_page(lv_event_t *e) {
    lv_scr_load(static_cast<lv_obj_t *>(lv_event_get_user_data(e)));
}
static void menu_item(const char *title, int y, lv_obj_t *page) {
    lv_obj_t *b = button(home_screen, title, 190, y, 420, 85);
    lv_obj_add_event_cb(b, open_page, LV_EVENT_CLICKED, page);
}

// ---------- 屏幕测试：纯色、彩条和棋盘格，不叠加文字污染黑色区域 ----------
static void solid_rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, ui_px(x), ui_px(y)); lv_obj_set_size(r, ui_px(w), ui_px(h));
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}
static void draw_pattern() {
    lv_obj_clean(test_content);
    static const uint32_t colors[] = {0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF,
                                      0xFFFF00, 0x00FFFF, 0xFF00FF};
    if (pattern < 5) solid_rect(test_content, 0, 0, 800, 800, colors[pattern]);
    else if (pattern == 5) {
        for (unsigned i = 0; i < 8; ++i) solid_rect(test_content, i * 100, 0, 100, 800, colors[i]);
    } else {
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x)
                solid_rect(test_content, x * 80, y * 80, 80, 80,
                           ((x + y) & 1) ? 0xFFFFFF : 0x000000);
    }
}
static void test_event(lv_event_t *e) {
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: test_long_press = false; break;
    case LV_EVENT_LONG_PRESSED:
        test_long_press = true;
        lv_scr_load(home_screen);
        break;
    case LV_EVENT_CLICKED:
        if (!test_long_press) { pattern = (pattern + 1) % 7; draw_pattern(); }
        break;
    default: break;
    }
}

// ---------- 小控件：仅更新 UI，不调用 Wi-Fi、BLE 或真实背光接口 ----------
static void switch_event(lv_event_t *e) {
    lv_obj_t *state = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    lv_label_set_text(state, on ? "ON (demo)" : "OFF (demo)");
    lv_obj_set_style_text_color(state, lv_color_hex(on ? GREEN : CYAN), 0);
}
static void switch_row(const char *name, int y) {
    label(widget_screen, name, 160, y, 170);
    lv_obj_t *state = label(widget_screen, "OFF (demo)", 330, y, 160);
    lv_obj_t *sw = lv_switch_create(widget_screen);
    lv_obj_set_pos(sw, ui_px(530), ui_px(y - 5)); lv_obj_set_size(sw, ui_px(90), ui_px(44));
    lv_obj_add_event_cb(sw, switch_event, LV_EVENT_VALUE_CHANGED, state);
}
static void value_event(lv_event_t *e) {
    lv_label_set_text_fmt(static_cast<lv_obj_t *>(lv_event_get_user_data(e)),
                         "Brightness (demo): %ld%%",
                         (long)lv_slider_get_value(lv_event_get_target(e)));
}
static void option_event(lv_event_t *e) {
    char value[40];
    lv_dropdown_get_selected_str(lv_event_get_target(e), value, sizeof(value));
    lv_label_set_text_fmt(static_cast<lv_obj_t *>(lv_event_get_user_data(e)), "Mode: %s", value);
}
static void create_widgets() {
    label(widget_screen, "CONTROL CENTER", 160, 90, 480, &lv_font_montserrat_28);
    label(widget_screen, "UI ONLY - no radio or hardware control", 130, 142, 540, &lv_font_montserrat_14);
    switch_row(LV_SYMBOL_WIFI "  Wi-Fi", 215);
    switch_row(LV_SYMBOL_BLUETOOTH "  BLE", 290);
    lv_obj_t *value = label(widget_screen, "Brightness (demo): 50%", 180, 385, 440);
    lv_obj_t *s = lv_slider_create(widget_screen);
    lv_obj_set_pos(s, ui_px(210), ui_px(445)); lv_obj_set_size(s, ui_px(380), ui_px(20));
    lv_slider_set_range(s, 0, 100); lv_slider_set_value(s, 50, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s, 20);
    lv_obj_add_event_cb(s, value_event, LV_EVENT_VALUE_CHANGED, value);
    lv_obj_t *mode = label(widget_screen, "Mode: Standard", 180, 505, 440);
    lv_obj_t *drop = lv_dropdown_create(widget_screen);
    lv_obj_set_pos(drop, ui_px(260), ui_px(555)); lv_obj_set_size(drop, ui_px(280), ui_px(55));
    lv_dropdown_set_options(drop, "Standard\nComfort\nPerformance");
    lv_obj_add_event_cb(drop, option_event, LV_EVENT_VALUE_CHANGED, mode);
    label(widget_screen, "Switches and values persist between pages", 150, 656, 500, &lv_font_montserrat_14);
    back_button(widget_screen);
}

// 相机按钮只提交请求；耗时硬件操作由主循环执行，避免阻塞 LVGL 任务。
static void camera_toggle(lv_event_t *) {
    if (camera_preview_busy()) return;
    camera_preview_request(!camera_preview_requested());
}
static void create_camera_page() {
    camera_screen = new_screen();
    label(camera_screen, "CAMERA", 180, 75, 440, &lv_font_montserrat_28);
    label(camera_screen, "OV5647 / CSI / RGB565", 180, 120, 440, &lv_font_montserrat_14);
    lv_obj_t *view = lv_img_create(camera_screen);
    lv_obj_set_pos(view, ui_px(200), ui_px(185));
    lv_obj_add_flag(view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *state = label(camera_screen, "Camera OFF", 150, 595, 500, &lv_font_montserrat_14);
    lv_obj_t *toggle = button(camera_screen, "CAMERA ON", 250, 654, 300, 55);
    lv_obj_add_event_cb(toggle, camera_toggle, LV_EVENT_CLICKED, nullptr);
    camera_preview_bind(view, state, lv_obj_get_child(toggle, 0));
    back_button(camera_screen);
}

// ---------- 开机页：LVGL 定时器推进，不在主循环中阻塞等待 ----------
static void boot_tick(lv_timer_t *timer) {
    lv_obj_t *bar = static_cast<lv_obj_t *>(timer->user_data);
    int value = lv_bar_get_value(bar) + 5;
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    if (value >= 100) {
        lv_scr_load(home_screen);
        lv_timer_del(timer);
    }
}
bool tp_test_ui_create() {
    if (home_screen || lv_disp_get_hor_res(nullptr) != 720 ||
        lv_disp_get_ver_res(nullptr) != 720) return false;
    home_screen = new_screen(); touch_screen = new_screen();
    widget_screen = new_screen(); test_screen = new_screen(); boot_screen = new_screen();
    if (!create_touch_page(touch_screen)) return false;
    back_button(touch_screen);
    create_widgets();
    create_camera_page();
    // 图案容器不接收点击，手势交给整个测试屏幕。
    test_content = lv_obj_create(test_screen);
    lv_obj_remove_style_all(test_content);
    lv_obj_set_size(test_content, ui_px(800), ui_px(800));
    lv_obj_clear_flag(test_content, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(test_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(test_screen, test_event, LV_EVENT_ALL, nullptr);
    draw_pattern();
    label(home_screen, "ESP32-P4", 180, 105, 440, &lv_font_montserrat_28);
    label(home_screen, "ROUND DISPLAY / LVGL UI DEMO", 150, 155, 500);
    menu_item("DISPLAY TEST", 210, test_screen);
    menu_item("TOUCH LAB", 310, touch_screen);
    menu_item("Wi-Fi / BLE / WIDGETS", 410, widget_screen);
    menu_item("CAMERA", 510, camera_screen);
    label(home_screen, "Display test: tap to change pattern", 140, 625, 520, &lv_font_montserrat_14);
    label(home_screen, "Long press anywhere to return home", 140, 650, 520, &lv_font_montserrat_14);
    label(home_screen, "720 x 720  /  CST3530", 200, 690, 400, &lv_font_montserrat_14);
    label(boot_screen, "HELLO", 200, 250, 400, &lv_font_montserrat_28);
    label(boot_screen, "HXESP  /  DISPLAY LAB", 150, 320, 500);
    label(boot_screen, "Starting UI demo...", 200, 450, 400, &lv_font_montserrat_14);
    lv_obj_t *bar = lv_bar_create(boot_screen);
    lv_obj_set_pos(bar, ui_px(250), ui_px(410)); lv_obj_set_size(bar, ui_px(300), ui_px(12));
    lv_bar_set_range(bar, 0, 100);
    lv_scr_load(boot_screen);
    return lv_timer_create(boot_tick, 100, bar) != nullptr;
}
