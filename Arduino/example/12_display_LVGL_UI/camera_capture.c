/*
 * ESP32-P4 相机采集层：
 * OV5647 RAW8 -> MIPI CSI -> ISP 去马赛克/RGB565 -> DMA 帧缓冲。
 * 本层编排传感器和接收端，不绘制 UI，不依赖 Arduino 或 LVGL。
 *
 * 前置条件：板级已初始化共享 I2C，并保持 PHY LDO3 2.5V 供电。
 * 主任务顺序：start -> acquire -> 使用只读帧 -> release -> ... -> stop。
 * 控制接口必须由单一任务串行调用；stats_lock 只保护与回调共享的数据，
 * 不代表整个启动/销毁流程可以被多个任务并发调用。
 * 正常关闭会删除 CSI/ISP、释放采集帧；下次开启完整重建。
 */
// Arduino 核心默认只编译 ERROR 日志，本模块保留诊断用 INFO。
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL 3
#include "camera_capture.h"
#include "ov5647_camera.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "driver/isp_core.h"
#include "driver/isp_demosaic.h"
#include "esp_cache.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define FRAME_BYTES (OV5647_FRAME_WIDTH * OV5647_FRAME_HEIGHT * 2U) // RGB565 每像素2字节，当前1280000字节/帧。
static esp_cam_ctlr_handle_t camera; // CSI 控制器句柄，NULL 表示未创建或已删除。
static isp_proc_handle_t isp;       // ISP 图像处理器句柄。
static void *buffers[3];            // 应用拥有的三个采集缓冲，不包含驱动内部备用缓冲。
// FREE 可分配；DMA 已交给接收端；READY 完整帧待消费；READING 已借给上层。
enum buffer_state { FREE, DMA, READY, READING };
static enum buffer_state states[3]; // 与 buffers 按下标一一对应，静态初值为 FREE。
static bool attempted, camera_started, faulted; // 本轮已尝试创建、CSI已启动、故障锁定。
static bool csi_enabled, isp_enabled, demosaic_enabled; // 记录成功的启用步骤，供部分失败清理使用。
static uint32_t generation;         // 创建轮次，用于检查反复开关和内存变化。
static camera_capture_stats_t stats; // 当前轮次的完成回调数、异常长度数和运行标志。
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED; // 任务/回调间的短临界区。
static const char *stage = "idle";  // 最近执行阶段；启动成功不代表已经收到首帧。
// 返回静态阶段字符串；与其他控制接口在同一任务调用。
const char *camera_capture_stage(void) { return stage; }
// 锁内只复制快照，锁外打印，避免串口日志延长临界区；不可从 ISR 调用。
void camera_capture_log(void) {
    camera_capture_stats_t snapshot;
    unsigned state_copy[3];
    portENTER_CRITICAL(&stats_lock);
    snapshot = stats;
    for (int i = 0; i < 3; ++i) state_copy[i] = states[i];
    portEXIT_CRITICAL(&stats_lock);
    ESP_LOGI("CAM", "stage=%s frames=%lu bad=%lu bytes=%lu buffers=%u/%u/%u (FREE/DMA/READY/READING=0/1/2/3)",
             stage, (unsigned long)snapshot.frames, (unsigned long)snapshot.wrong_size,
             (unsigned long)snapshot.last_bytes, state_copy[0], state_copy[1], state_copy[2]);
}

// 三缓冲所有权：FREE -> DMA -> READY -> READING -> FREE。
// 显示较慢时丢弃旧 READY，只保留最新帧；不覆盖正在复制的 READING。
static bool IRAM_ATTR get_transaction(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user) {
    // 驱动请求下一块目标缓冲时调用；按中断上下文约束编写，不分配内存、不做I2C。
    (void)handle; (void)user;
    trans->buffer = NULL;
    trans->buflen = 0; // 无可用缓冲时由驱动使用 backup buffer。
    portENTER_CRITICAL_ISR(&stats_lock);
    int chosen = -1; // 优先空闲缓冲；没有空闲时允许覆盖尚未借出的旧完整帧。
    for (int i = 0; i < 3; ++i) if (states[i] == FREE) { chosen = i; break; }
    if (chosen < 0) for (int i = 0; i < 3; ++i) if (states[i] == READY) { chosen = i; break; }
    if (chosen >= 0) {
        states[chosen] = DMA;
        trans->buffer = buffers[chosen];
        trans->buflen = FRAME_BYTES;
    }
    portEXIT_CRITICAL_ISR(&stats_lock);
    return false; // 没有唤醒更高优先级任务，无需请求调度切换；不是“分配失败”。
}
// 帧完成回调：只统计并交接缓冲，不在回调中复制整幅图像或刷新屏幕。
static bool IRAM_ATTR frame_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user) {
    (void)handle; (void)user;
    portENTER_CRITICAL_ISR(&stats_lock);
    ++stats.frames; // 包括坏长度/备用缓冲的完成回调，不等于实际显示帧数。
    stats.last_bytes = trans->received_size;
    if (trans->received_size != FRAME_BYTES) ++stats.wrong_size;
    for (int i = 0; i < 3; ++i) {
        if (buffers[i] != trans->buffer || states[i] != DMA) continue; // 只交接本应用的在途帧。
        if (trans->received_size == FRAME_BYTES) {
            for (int j = 0; j < 3; ++j) if (states[j] == READY) states[j] = FREE; // 丢弃旧待消费帧，保留最新帧。
            states[i] = READY;
        } else states[i] = FREE; // 长度不符不交给上层显示。
        break;
    }
    portEXIT_CRITICAL_ISR(&stats_lock);
    return false;
}
// 非阻塞借帧：没有 READY 返回 NOT_FOUND，上层稍后再试，不在此等待。
// 成功返回后必须 release；缓存同步失败会自动归还，输出指针和大小清零。
esp_err_t camera_capture_acquire(const void **buffer, size_t *size) {
    if (!buffer || !size) return ESP_ERR_INVALID_ARG;
    *buffer = NULL; *size = 0;
    if (!camera_started || faulted) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&stats_lock);
    for (int i = 0; i < 3; ++i) if (states[i] == READY) {
        states[i] = READING; *buffer = buffers[i]; *size = FRAME_BYTES; break; // 先占有再离开锁，防止DMA重用。
    }
    portEXIT_CRITICAL(&stats_lock);
    if (!*buffer) return ESP_ERR_NOT_FOUND;
    // DMA 写入后先使 CPU 缓存失效；借出缓冲只读。
    esp_err_t err = esp_cache_msync((void *)*buffer, *size,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK) { camera_capture_release(*buffer); *buffer = NULL; *size = 0; }
    return err;
}
// 归还所有权，不释放内存；归还后上层不得继续读取或让异步显示持有该指针。
// 未找到匹配的 READING 缓冲时不做操作。
void camera_capture_release(const void *buffer) {
    portENTER_CRITICAL(&stats_lock);
    for (int i = 0; i < 3; ++i)
        if (buffers[i] == buffer && states[i] == READING) { states[i] = FREE; break; }
    portEXIT_CRITICAL(&stats_lock);
}
// 获取一致的统计快照，不触发采集，也不清零计数。
void camera_capture_stats(camera_capture_stats_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&stats_lock);
    *out = stats;
    portEXIT_CRITICAL(&stats_lock);
}
/*
 * 关闭顺序：
 * 传感器停流 -> CSI stop/disable -> 去马赛克disable -> ISP disable
 * -> 删除CSI -> 删除ISP -> 释放应用帧。
 * enable/disable 控制驱动状态，delete 才释放句柄和驱动资源，二者不能互相替代。
 * 每一步仅在对应状态允许时执行；err 优先保留前面发生的错误。
 * 不关闭共享PHY电源、不删除共享I2C、不影响LCD/LVGL资源。
 */
esp_err_t camera_capture_stop(void) {
    if (!attempted) return ESP_OK; // 重复关闭不再向传感器发送命令。
    // 禁止在消费者仍持帧时停止或重置缓冲状态。
    bool borrowed = false;
    portENTER_CRITICAL(&stats_lock);
    for (int i = 0; i < 3; ++i) borrowed |= states[i] == READING;
    portEXIT_CRITICAL(&stats_lock);
    if (borrowed) return ESP_ERR_INVALID_STATE;
    stage = "sensor stream off";
    esp_err_t err = ov5647_camera_stream(false);
    ESP_LOGI("CAM", "sensor OFF: %s", esp_err_to_name(err));
    // 给当前曝光/传输留下收尾时间；真正的 DMA 停止仍由驱动 API 保证。
    if (err == ESP_OK && camera_started) vTaskDelay(pdMS_TO_TICKS(80) + 1);
    if (camera_started) {
        stage = "CSI stop";
        esp_err_t stop = esp_cam_ctlr_stop(camera);
        ESP_LOGI("CAM", "CSI stop: %s", esp_err_to_name(stop));
        if (stop == ESP_OK) camera_started = false;
        if (err == ESP_OK) err = stop; // 即使停流失败仍尝试停接收，但保留最先的错误。
    }
    portENTER_CRITICAL(&stats_lock);
    stats.running = camera_started;
    portEXIT_CRITICAL(&stats_lock);
    // 不只切换 stream：重新启用前让 CSI/ISP 回到 disabled 生命周期状态。
    if (!camera_started && csi_enabled) {
        stage = "CSI disable";
        esp_err_t result = esp_cam_ctlr_disable(camera);
        ESP_LOGI("CAM", "CSI disable: %s", esp_err_to_name(result));
        if (result == ESP_OK) csi_enabled = false;
        if (err == ESP_OK) err = result;
    }
    if (!camera_started && !csi_enabled && demosaic_enabled) {
        stage = "demosaic disable";
        esp_err_t result = esp_isp_demosaic_disable(isp);
        ESP_LOGI("CAM", "demosaic disable: %s", esp_err_to_name(result));
        if (result == ESP_OK) demosaic_enabled = false;
        if (err == ESP_OK) err = result;
    }
    if (!camera_started && !csi_enabled && !demosaic_enabled && isp_enabled) {
        stage = "ISP disable";
        esp_err_t result = esp_isp_disable(isp);
        ESP_LOGI("CAM", "ISP disable: %s", esp_err_to_name(result));
        if (result == ESP_OK) isp_enabled = false;
        if (err == ESP_OK) err = result;
    }
    // 任何停止失败均保留句柄和帧内存，禁止继续重建。
    if (err != ESP_OK) {
        faulted = true;
        camera_capture_log();
        return err;
    }
    // 删除控制器后其内部备用帧/中断/DMA资源由驱动回收。
    // 应用通过 alloc_buffer 获得的三帧由应用单独释放，不触碰驱动备用帧。
    if (camera) {
        stage = "CSI delete";
        err = esp_cam_ctlr_del(camera);
        ESP_LOGI("CAM", "CSI delete: %s", esp_err_to_name(err));
        if (err != ESP_OK) { faulted = true; return err; }
        camera = NULL; // 只有删除成功才清空，失败保留句柄以便诊断。
    }
    if (isp) {
        stage = "ISP delete";
        err = esp_isp_del_processor(isp);
        ESP_LOGI("CAM", "ISP delete: %s", esp_err_to_name(err));
        if (err != ESP_OK) { faulted = true; return err; }
        isp = NULL;
    }
    // 两个驱动均已删除，此时不再有 DMA/回调能访问应用帧。
    for (int i = 0; i < 3; ++i) {
        heap_caps_free(buffers[i]);
        buffers[i] = NULL;
        states[i] = FREE;
    }
    attempted = false; // 正常关闭允许下次创建；故障锁 faulted 不在此清除。
    stage = "released";
    ESP_LOGI("CAM", "generation=%lu released; PSRAM free=%u largest=%u",
             (unsigned long)generation, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    camera_capture_log();
    return err;
}

// 每个初始化步骤先记录阶段，再执行；遇错统一跳到 fail，返回原始错误码。
#define STEP(name, call) do { stage = name; err = (call); if (err != ESP_OK) goto fail; } while (0)
// 初始化只保证接收链路和输出流已启动，首帧及连续性仍由上层超时机制验证。
esp_err_t camera_capture_start(void) {
    esp_log_level_set("CAM", ESP_LOG_INFO);
    esp_err_t err;
    // 每次开启都走同一条全新构建路径，不复用停止后的 CSI/ISP 句柄。
    if (attempted || faulted || camera || isp) return ESP_ERR_INVALID_STATE;
    attempted = true;
    ++generation;
    portENTER_CRITICAL(&stats_lock);
    stats = (camera_capture_stats_t){0};
    portEXIT_CRITICAL(&stats_lock);
    ESP_LOGI("CAM", "build generation=%lu PSRAM free=%u largest=%u",
             (unsigned long)generation, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    // CSI接收：两条数据Lane，每Lane速率来自模式参数；RAW8输入，最终RGB565输出。
    esp_cam_ctlr_csi_config_t csi = {
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .ctlr_id = 0, .h_res = OV5647_FRAME_WIDTH, .v_res = OV5647_FRAME_HEIGHT,
        .data_lane_num = 2, .lane_bit_rate_mbps = OV5647_LANE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .queue_items = 1, // 驱动事务队列深度，不是应用帧缓冲数量。
    };
    // ISP负责像素转换，CSI输出配置本身不能代替ISP去马赛克处理。
    esp_isp_processor_cfg_t processor = {
        .clk_src = ISP_CLK_SRC_DEFAULT,
        .clk_hz = 80000000, // ISP处理时钟80MHz，不是传感器晶振或MIPI Lane速率。
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .h_res = OV5647_FRAME_WIDTH, .v_res = OV5647_FRAME_HEIGHT,
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG, // 与当前传感器模式匹配的Bayer排列。
    };
    // 去马赛克使用梯度比例2和周边数据边界填充；未配置完整AE/AWB调优。
    esp_isp_demosaic_config_t demosaic = {
        .grad_ratio = {.integer = 2},
        .padding_mode = ISP_DEMOSAIC_EDGE_PADDING_MODE_SRND_DATA,
    };
    // 一个回调供给目标缓冲，另一个回调提交已完成帧。
    esp_cam_ctlr_evt_cbs_t callbacks = {
        .on_get_new_trans = get_transaction, .on_trans_finished = frame_finished,
    };
    STEP("OV5647 mode 25MHz RAW8 800x800", ov5647_camera_configure());
    // 本集成工程由 Board 持有 LDO3 2.5V；显示启动后才允许调用，不重复申请或释放。
    STEP("CSI controller", esp_cam_new_csi_ctlr(&csi, &camera));
    stage = "RGB565 DMA buffer";
    // 从PSRAM申请适合相机DMA访问的缓冲；部分申请失败也进入统一清理路径。
    for (int i = 0; i < 3; ++i) {
        buffers[i] = esp_cam_ctlr_alloc_buffer(camera, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!buffers[i]) { err = ESP_ERR_NO_MEM; goto fail; }
    }
    STEP("ISP processor", esp_isp_new_processor(&processor, &isp));
    STEP("ISP demosaic configure", esp_isp_demosaic_configure(isp, &demosaic));
    STEP("ISP enable", esp_isp_enable(isp));
    isp_enabled = true;
    STEP("ISP demosaic enable", esp_isp_demosaic_enable(isp));
    demosaic_enabled = true;
    STEP("CSI callbacks", esp_cam_ctlr_register_event_callbacks(camera, &callbacks, NULL));
    STEP("CSI enable", esp_cam_ctlr_enable(camera));
    csi_enabled = true;
    STEP("CSI start", esp_cam_ctlr_start(camera)); // 先准备接收端，再让传感器输出。
    camera_started = true;
    STEP("OV5647 stream on", ov5647_camera_stream(true));
    portENTER_CRITICAL(&stats_lock);
    stats.running = true;
    portEXIT_CRITICAL(&stats_lock);
    stage = "running (await frame callbacks)";
    return ESP_OK;
fail:
    ESP_LOGE("CAM", "start failed at %s: %s", stage, esp_err_to_name(err));
    faulted = true; // 初始化失败即使清理成功也不自动重试。
    // 仅在停止/删除均成功后回收资源；任何一步失败都保留余下内存。
    (void)camera_capture_stop(); // 清理结果另有日志；函数仍返回触发失败的原始 err。
    return err;
}
