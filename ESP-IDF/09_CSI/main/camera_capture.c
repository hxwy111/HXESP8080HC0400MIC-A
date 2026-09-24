#include "camera_capture.h"
#include "ov5647_camera.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "driver/isp_core.h"
#include "driver/isp_demosaic.h"
#include "mipi_power.h"
#include "esp_cache.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"

#define FRAME_BYTES (OV5647_FRAME_WIDTH * OV5647_FRAME_HEIGHT * 2U)
static esp_cam_ctlr_handle_t camera;
static isp_proc_handle_t isp;
static void *buffers[3];
enum buffer_state { FREE, DMA, READY, READING };
static enum buffer_state states[3];
static bool attempted, camera_started;
static camera_capture_stats_t stats;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *stage = "idle";
const char *camera_capture_stage(void) { return stage; }

// 三缓冲所有权：FREE -> DMA -> READY -> READING -> FREE。
// 显示较慢时丢弃旧 READY，只保留最新帧；不覆盖正在复制的 READING。
static bool IRAM_ATTR get_transaction(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user) {
    (void)handle; (void)user;
    trans->buffer = NULL;
    trans->buflen = 0; // 无可用缓冲时由驱动使用 backup buffer。
    portENTER_CRITICAL_ISR(&stats_lock);
    int chosen = -1;
    for (int i = 0; i < 3; ++i) if (states[i] == FREE) { chosen = i; break; }
    if (chosen < 0) for (int i = 0; i < 3; ++i) if (states[i] == READY) { chosen = i; break; }
    if (chosen >= 0) {
        states[chosen] = DMA;
        trans->buffer = buffers[chosen];
        trans->buflen = FRAME_BYTES;
    }
    portEXIT_CRITICAL_ISR(&stats_lock);
    return false;
}
static bool IRAM_ATTR frame_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user) {
    (void)handle; (void)user;
    portENTER_CRITICAL_ISR(&stats_lock);
    ++stats.frames;
    stats.last_bytes = trans->received_size;
    if (trans->received_size != FRAME_BYTES) ++stats.wrong_size;
    for (int i = 0; i < 3; ++i) {
        if (buffers[i] != trans->buffer || states[i] != DMA) continue;
        if (trans->received_size == FRAME_BYTES) {
            for (int j = 0; j < 3; ++j) if (states[j] == READY) states[j] = FREE;
            states[i] = READY;
        } else states[i] = FREE;
        break;
    }
    portEXIT_CRITICAL_ISR(&stats_lock);
    return false;
}
esp_err_t camera_capture_acquire(const void **buffer, size_t *size) {
    if (!buffer || !size) return ESP_ERR_INVALID_ARG;
    *buffer = NULL; *size = 0;
    portENTER_CRITICAL(&stats_lock);
    for (int i = 0; i < 3; ++i) if (states[i] == READY) {
        states[i] = READING; *buffer = buffers[i]; *size = FRAME_BYTES; break;
    }
    portEXIT_CRITICAL(&stats_lock);
    if (!*buffer) return ESP_ERR_NOT_FOUND;
    // DMA 写入后先使 CPU 缓存失效；借出缓冲只读。
    esp_err_t err = esp_cache_msync((void *)*buffer, *size,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK) { camera_capture_release(*buffer); *buffer = NULL; *size = 0; }
    return err;
}
void camera_capture_release(const void *buffer) {
    portENTER_CRITICAL(&stats_lock);
    for (int i = 0; i < 3; ++i)
        if (buffers[i] == buffer && states[i] == READING) { states[i] = FREE; break; }
    portEXIT_CRITICAL(&stats_lock);
}
void camera_capture_stats(camera_capture_stats_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&stats_lock);
    *out = stats;
    portEXIT_CRITICAL(&stats_lock);
}
esp_err_t camera_capture_stop(void) {
    esp_err_t err = ov5647_camera_stream(false);
    if (camera_started) {
        esp_err_t stop = esp_cam_ctlr_stop(camera);
        if (stop == ESP_OK) camera_started = false;
        if (err == ESP_OK) err = stop;
    }
    portENTER_CRITICAL(&stats_lock);
    stats.running = camera_started;
    portEXIT_CRITICAL(&stats_lock);
    return err;
}

#define STEP(name, call) do { stage = name; err = (call); if (err != ESP_OK) goto fail; } while (0)
esp_err_t camera_capture_start(void) {
    if (attempted) return ESP_ERR_INVALID_STATE;
    attempted = true;
    esp_err_t err;
    esp_cam_ctlr_csi_config_t csi = {
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .ctlr_id = 0, .h_res = OV5647_FRAME_WIDTH, .v_res = OV5647_FRAME_HEIGHT,
        .data_lane_num = 2, .lane_bit_rate_mbps = OV5647_LANE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .queue_items = 1,
    };
    esp_isp_processor_cfg_t processor = {
        .clk_src = ISP_CLK_SRC_DEFAULT,
        .clk_hz = 80000000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .h_res = OV5647_FRAME_WIDTH, .v_res = OV5647_FRAME_HEIGHT,
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG,
    };
    esp_isp_demosaic_config_t demosaic = {
        .grad_ratio = {.integer = 2},
        .padding_mode = ISP_DEMOSAIC_EDGE_PADDING_MODE_SRND_DATA,
    };
    esp_cam_ctlr_evt_cbs_t callbacks = {
        .on_get_new_trans = get_transaction, .on_trans_finished = frame_finished,
    };
    STEP("OV5647 mode 25MHz RAW8 800x800", ov5647_camera_configure());
    // CSI 与 DSI 共用 PHY LDO3 2.5V，由 mipi_power 模块统一申请并保持供电。
    STEP("shared PHY power", mipi_power_init());
    STEP("CSI controller", esp_cam_new_csi_ctlr(&csi, &camera));
    stage = "RGB565 DMA buffer";
    for (int i = 0; i < 3; ++i) {
        buffers[i] = esp_cam_ctlr_alloc_buffer(camera, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!buffers[i]) { err = ESP_ERR_NO_MEM; goto fail; }
    }
    STEP("ISP processor", esp_isp_new_processor(&processor, &isp));
    STEP("ISP demosaic configure", esp_isp_demosaic_configure(isp, &demosaic));
    STEP("ISP enable", esp_isp_enable(isp));
    STEP("ISP demosaic enable", esp_isp_demosaic_enable(isp));
    STEP("CSI callbacks", esp_cam_ctlr_register_event_callbacks(camera, &callbacks, NULL));
    STEP("CSI enable", esp_cam_ctlr_enable(camera));
    STEP("CSI start", esp_cam_ctlr_start(camera));
    camera_started = true;
    STEP("OV5647 stream on", ov5647_camera_stream(true));
    portENTER_CRITICAL(&stats_lock);
    stats.running = true;
    portEXIT_CRITICAL(&stats_lock);
    stage = "running (await frame callbacks)";
    return ESP_OK;
fail:
    // 不释放可能仍由 DMA 使用的内存。失败后关闭视频/接收，保留资源，复位后重试。
    (void)camera_capture_stop();
    return err;
}
