#include "camera_bringup.h"
#include "ov5647_camera.h"
#include "bsp_i2c.h"
#include "camera_capture.h"
#include "camera_display.h"
#include <Arduino.h>
static bool bus_ready;
static bool capture_started;
static uint32_t last_stats_ms, last_frame_ms, previous_frames;
static bool display_ready;
static uint32_t displayed;

static void show_info() {
    uint16_t id;
    esp_err_t err = ov5647_camera_detect(&id);
    Serial.printf("CAM: address=0x36 id=0x%04X result=%s\n", (unsigned)id, esp_err_to_name(err));
    if (err != ESP_OK) return;
    ov5647_diagnostics_t info;
    err = ov5647_camera_read_diagnostics(&info);
    if (err != ESP_OK) {
        Serial.printf("CAM: register read failed: %s\n", esp_err_to_name(err));
        return;
    }
    Serial.printf("CAM: stream[0100]=%02X format[3034]=%02X\n", info.stream, info.format);
    Serial.printf("CAM: PLL[3035/3036/303C/3106]=%02X/%02X/%02X/%02X\n",
                  info.pll[0], info.pll[1], info.pll[2], info.pll[3]);
    Serial.printf("CAM: output=%u x %u HTS=%u VTS=%u mirror[3820/3821]=%02X/%02X MIPI[4800]=%02X\n",
                  info.width, info.height, info.hts, info.vts,
                  info.mirror[0], info.mirror[1], info.mipi_control);
    Serial.println("CAM: register access OK; register values alone do not prove captured frames.");
    Serial.println("CAM: these registers do not measure the physical oscillator frequency.");
}
static void scan() {
    unsigned count = 0;
    for (unsigned address = 0x08; address <= 0x77; ++address) {
        esp_err_t err = bsp_i2c_probe(address);
        if (err == ESP_OK) { Serial.printf("I2C: 0x%02X\n", address); ++count; }
        else if (err == ESP_ERR_TIMEOUT) {
            Serial.printf("I2C: timeout at 0x%02X, scan stopped; check bus/power.\n", address);
            return;
        }
    }
    Serial.printf("I2C: %u responding address(es); not all are cameras.\n", count);
}
void camera_bringup_begin() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\nOV5647 bring-up: SDA=7 SCL=8; sensor board pin15=3.3V");
    esp_err_t err = bsp_i2c_init(0, 7, 8, 100000);
    if (err != ESP_OK) {
        Serial.printf("I2C init failed: %s\n", esp_err_to_name(err));
        return;
    }
    bus_ready = true;
    delay(200);
    scan();
    show_info();
    Serial.println("CAM: assuming onboard 25MHz; RAW8 800x800 -> ISP RGB565 -> LCD 720x720");
    Serial.println("LCD: JD9365 2 lanes, RGB565, 30MHz pixel clock; 800x800 camera frames are downscaled");
    err = camera_display_init();
    if (err != ESP_OK) {
        Serial.printf("DISPLAY init failed: %s; reset to retry.\n", esp_err_to_name(err));
        return;
    }
    display_ready = true;
    err = camera_capture_start();
    if (err != ESP_OK) {
        Serial.printf("CAPTURE: init failed at [%s]: %s; reset board to retry.\n",
                      camera_capture_stage(), esp_err_to_name(err));
    } else {
        capture_started = true;
        last_stats_ms = last_frame_ms = millis();
        Serial.println("PREVIEW: waiting for first frame; LCD backlight opens after first copy.");
        show_info();
    }
    Serial.println("Commands: i=read registers, x=stop capture; s/r allowed only after stopping");
    Serial.println("Power OFF before connecting/removing the camera ribbon.");
}
void camera_bringup_poll() {
    if (capture_started && display_ready) {
        const void *pixels = nullptr;
        size_t bytes = 0;
        esp_err_t err = camera_capture_acquire(&pixels, &bytes);
        if (err == ESP_OK) {
            err = camera_display_show(pixels, bytes);
            camera_capture_release(pixels); // 包括显示失败路径，始终归还源帧。
            if (err == ESP_OK) ++displayed;
        }
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            Serial.printf("PREVIEW failed: %s; stop=%s\n", esp_err_to_name(err),
                          esp_err_to_name(camera_capture_stop()));
            display_ready = false;
            camera_capture_stats_t status;
            camera_capture_stats(&status);
            capture_started = status.running;
        }
    }
    if (bus_ready && Serial.available()) {
        char c = Serial.read();
        if (c == 'i') show_info();
        else if (c == 'x') {
            esp_err_t stop = camera_capture_stop();
            Serial.printf("CAPTURE stop: %s\n", esp_err_to_name(stop));
            if (stop == ESP_OK) capture_started = false;
        }
        else if ((c == 'r' || c == 's') && capture_started)
            Serial.println("Stop capture with x first; reset/scanning during capture is disabled.");
        else if (c == 's') scan();
        else if (c == 'r') {
            uint16_t id;
            esp_err_t err = ov5647_camera_detect(&id);
            if (err == ESP_OK) err = ov5647_camera_reset();
            Serial.printf("CAM: software reset: %s\n", esp_err_to_name(err));
            if (err == ESP_OK) show_info();
        }
    }
    if (capture_started && millis() - last_stats_ms >= 1000) {
        uint32_t now = millis();
        camera_capture_stats_t status;
        camera_capture_stats(&status);
        uint32_t delta = status.frames - previous_frames;
        float fps = delta * 1000.0f / (now - last_stats_ms);
        Serial.printf("CAPTURE: frames=%lu fps=%.1f camera_bytes=%lu expected=1280000 bad_size=%lu\n",
                      (unsigned long)status.frames, fps, (unsigned long)status.last_bytes,
                      (unsigned long)status.wrong_size);
        Serial.printf("PREVIEW: copied=%lu free_psram=%lu\n",
                      (unsigned long)displayed, (unsigned long)ESP.getFreePsram());
        if (delta) last_frame_ms = now;
        previous_frames = status.frames;
        last_stats_ms = now;
        if (now - last_frame_ms >= 5000) {
            esp_err_t stop = camera_capture_stop();
            Serial.printf("CAPTURE: no new frame for 5s; stop=%s. Check clock/ribbon/PHY/mode, reset to retry.\n",
                          esp_err_to_name(stop));
            if (stop == ESP_OK) capture_started = false;
            last_frame_ms = now;
        }
    }
    delay(10);
}
