/*
 * CST3530 触摸驱动：只处理硬件协议和触点事件，不依赖 Arduino 或 LVGL。
 *
 * 调用顺序：bsp_i2c_init -> cst3530_init -> cst3530_read_info
 *          -> cst3530_start -> 主循环持续调用 cst3530_poll。
 * 数据路径：INT 通知 -> I2C 读帧/应答 -> 校验解析 -> 状态比较 -> 用户回调。
 *
 * 本文件使用一组静态状态，只支持一个触摸设备。
 * 除 ISR 外，驱动接口应在同一个任务中串行调用；回调不可重入驱动。
 * 中断锁仅保护通知标志，不代表整个驱动支持多任务并发访问。
 */
#include "cst3530.h"          // 对外接口、触点/事件/诊断结构和设备地址。
#include "bsp_i2c.h"
#include <string.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_attr.h"

// ---------- 驱动状态：硬件初始化与事件上报分为两个阶段 ----------
static bool initialized;                       // 硬件复位及地址探测是否成功。
static bool reporting;                         // 是否已经注册回调并启用触摸上报。
static uint16_t report_width, report_height;    // 有效坐标范围，不在此处旋转或缩放。
static cst3530_callback_t report_callback;      // 用户注册的事件接收函数。
static void *callback_user;                    // 原样传给回调的用户上下文。
static cst3530_point_t active_points[16];       // 按 4 位 ID 索引，不代表支持 16 指同时触摸。
static int64_t retry_after_us;                 // 发生真实错误后，下次允许读帧的微秒时间。
static unsigned failed_frames;                // 连续失败的读帧轮次，饱和到 5。
static int tp_int_gpio = -1;                   // 初始化时保存的 INT 引脚号。
static cst3530_diagnostics_t diagnostics;      // 最近一轮读帧的原始数据及结果快照。
static portMUX_TYPE irq_lock = portMUX_INITIALIZER_UNLOCKED; // ISR 与任务共享的短临界区。
static volatile bool irq_pending;             // 至少发生过一次下降沿；不是中断计数器。

// ISR 只发布通知，不做 I2C、日志、解析或回调。锁保护跨核标志读写。
static void IRAM_ATTR tp_irq_handler(void *arg) {
    (void)arg;
    portENTER_CRITICAL_ISR(&irq_lock);
    irq_pending = true;
    portEXIT_CRITICAL_ISR(&irq_lock);
}

// 原子地取走并清除通知；多个下降沿可以合并，待处理报告由 INT 电平兜底判断。
static bool take_irq_notification(void) {
    portENTER_CRITICAL(&irq_lock);
    bool pending = irq_pending;
    irq_pending = false;
    portEXIT_CRITICAL(&irq_lock);
    return pending;
}

// 只复制诊断快照，不访问芯片；应与 poll 在同一任务中调用。
esp_err_t cst3530_get_diagnostics(cst3530_diagnostics_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!reporting || !diagnostics.sequence) return ESP_ERR_INVALID_STATE;
    *out = diagnostics;
    return ESP_OK;
}
static void wait_ms(unsigned ms) {
    // 向上取整，保证不短于芯片规定的等待时间。
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}
// 芯片返回的多字节数值按小端排列：低地址字节是低有效位。
// 按字节拼接，避免未对齐指针强转，也不依赖 CPU 的字节序。
static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
// 命令与返回数值的字节序不同：命令按高字节先发，例如 D0 00 02 AB。
// I2C 收发和总线互斥交给 bsp_i2c，本层只定义协议字节。
static esp_err_t command(uint32_t value) {
    const uint8_t bytes[] = {value >> 24, value >> 16, value >> 8, value};
    return bsp_i2c_transfer(CST3530_I2C_ADDRESS, bytes, sizeof(bytes), NULL, 0);
}

// 第一阶段：验证引脚 -> INT 输入且禁中断 -> RST 低 10ms/高后 200ms -> 探测 0x58。
// 必须由上层先初始化 I2C；这里地址 ACK 只证明设备响应，不等于型号验证成功。
esp_err_t cst3530_init(int reset_gpio, int interrupt_gpio) {
    // 运行中的驱动持有中断处理器，禁止未经停止流程重新初始化。
    if (reporting) return ESP_ERR_INVALID_STATE;
    initialized = false;
    reporting = false;
    memset(&diagnostics, 0, sizeof(diagnostics));
    if (!GPIO_IS_VALID_OUTPUT_GPIO(reset_gpio) || !GPIO_IS_VALID_GPIO(interrupt_gpio) ||
        reset_gpio == interrupt_gpio) return ESP_ERR_INVALID_ARG;
    // 电源由板级供电提供；不操作 LCD_RESET，不套用 GT911 的地址选择时序。
    esp_err_t err = gpio_set_direction((gpio_num_t)interrupt_gpio, GPIO_MODE_INPUT);
    if (err == ESP_OK) err = gpio_set_intr_type((gpio_num_t)interrupt_gpio, GPIO_INTR_DISABLE);
    if (err == ESP_OK) err = gpio_set_level((gpio_num_t)reset_gpio, 0);
    if (err == ESP_OK) err = gpio_set_direction((gpio_num_t)reset_gpio, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) return err;
    tp_int_gpio = interrupt_gpio;
    wait_ms(10);
    err = gpio_set_level((gpio_num_t)reset_gpio, 1);
    if (err != ESP_OK) return err;
    wait_ms(200);
    err = bsp_i2c_probe(CST3530_I2C_ADDRESS);
    initialized = err == ESP_OK;
    return err;
}

/*
 * 纯内存解析函数，不读 I2C、不发送 ACK、不调用用户回调。
 * 帧格式：
 *   raw[0..1]：小端 16 位累加校验值；
 *   raw[2]：报告类型，0xFF 表示本驱动支持的坐标报告；
 *   raw[3]：高 4 位按键记录数，低 4 位触点记录数；
 *   raw[4..]：先按键、后触点，每条记录 5 字节。
 * 校验算法是 0x55 加所有记录字节（16 位累加），不是多项式 CRC；
 * 校验不符借用 ESP_ERR_INVALID_CRC 错误码表示。
 * 按键记录参与长度/校验计算，但本驱动不产生按键事件。
 */
esp_err_t cst3530_parse_frame(const uint8_t *raw, size_t size, uint16_t width,
                             uint16_t height, cst3530_frame_t *frame) {
    if (!raw || !frame || !width || !height) return ESP_ERR_INVALID_ARG;
    memset(frame, 0, sizeof(*frame));
    if (size < 4) return ESP_ERR_INVALID_SIZE;
    unsigned fingers = raw[3] & 0x0F, keys = raw[3] >> 4;
    unsigned records = fingers + keys;
    if (records > CST3530_MAX_RECORDS || size < 4 + records * 5)
        return ESP_ERR_INVALID_SIZE;
    uint16_t sum = 0x55;
    for (unsigned i = 0; i < records * 5; ++i) sum += raw[4 + i];
    if (sum != read_le16(raw)) return ESP_ERR_INVALID_CRC;
    // 不把手势、接近事件或其他未知报告解释成松手。
    if (raw[2] != 0xFF) return ESP_ERR_NOT_SUPPORTED;
    cst3530_frame_t parsed = {0};
    uint16_t seen_ids = 0; // 位图检测同一帧内重复 ID，防止同一触点被重复解释。
    for (unsigned i = 0; i < fingers; ++i) {
        const uint8_t *p = raw + 4 + (keys + i) * 5; // 跳过帧头和按键记录。
        // p[0]/p[1] 为 X/Y 低 8 位；p[3] 的低/高半字节分别补足 X/Y 高 4 位。
        // p[2] 为压力；p[4] 低半字节为 ID，高半字节表达按压事件状态。
        cst3530_point_t *point = &parsed.points[i];
        point->id = p[4] & 0x0F;
        point->pressed = (p[4] >> 4) != 0; // 与厂家 report() 的 event 语义一致。
        point->x = p[0] | ((uint16_t)(p[3] & 0x0F) << 8);
        point->y = p[1] | ((uint16_t)(p[3] & 0xF0) << 4);
        point->pressure = p[2];
        if (seen_ids & (1U << point->id)) return ESP_ERR_INVALID_RESPONSE;
        seen_ids |= 1U << point->id;
        // 抬手包坐标可能无效，UP 使用上一次有效坐标，不因抬手坐标越界拒绝释放。
        if (point->pressed && (point->x >= width || point->y >= height))
            return ESP_ERR_INVALID_RESPONSE;
    }
    parsed.count = fingers;
    *frame = parsed; // 整帧通过才提交，不把半帧交给上层。
    return ESP_OK;
}

// 读帧事务：请求报告 -> 先读 9 字节 -> 按记录数补读 -> 解析 -> 发送结束命令。
// 一轮最多尝试两次；上层连续失败计数按本函数调用轮次计，不按内部尝试次数计。
static esp_err_t read_touch_frame(cst3530_frame_t *frame) {
    const uint8_t request[] = {0xD0, 0x07, 0x00, 0x00};
    esp_err_t result = ESP_FAIL;
    ++diagnostics.sequence;
    if (!diagnostics.sequence) ++diagnostics.sequence; // 0 保留为“还未执行读帧”。
    diagnostics.attempts = 0;
    memset(diagnostics.attempt, 0, sizeof(diagnostics.attempt));
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        cst3530_attempt_diag_t *diag = &diagnostics.attempt[attempt];
        diagnostics.attempts = attempt + 1;
        diag->int_before = gpio_get_level((gpio_num_t)tp_int_gpio);
        diag->int_after_ack = -1; // 未应答（首次失败后直接重试）。
        uint8_t raw[4 + CST3530_MAX_RECORDS * 5] = {0};
        size_t size = 9; // 先读 4 字节头和第一条 5 字节记录。
        result = bsp_i2c_transfer(CST3530_I2C_ADDRESS, request, sizeof(request), raw, size);
        if (result == ESP_OK) {
            diag->received_size = 9;
            unsigned records = (raw[3] & 0x0F) + (raw[3] >> 4);
            if (records > CST3530_MAX_RECORDS) result = ESP_ERR_INVALID_SIZE;
            else if (records > 1) {
                size = 4 + records * 5;
                // 继续读取芯片内部读指针后的数据，不重新发送报告命令。
                result = bsp_i2c_transfer(CST3530_I2C_ADDRESS, NULL, 0, raw + 9, size - 9);
                if (result == ESP_OK) diag->received_size = size;
            }
            if (result == ESP_OK)
                result = cst3530_parse_frame(raw, size, report_width, report_height, frame);
        }
        diag->int_after_read = gpio_get_level((gpio_num_t)tp_int_gpio);
        diag->result = result;
        memcpy(diag->raw, raw, diag->received_size); // 只记录确认读取成功的部分。
        if (diag->received_size >= 4) {
            unsigned count = (raw[3] & 15) + (raw[3] >> 4);
            if (count <= CST3530_MAX_RECORDS && diag->received_size >= 4 + count * 5) {
                diag->checksum_available = true;
                diag->checksum_received = read_le16(raw);
                diag->checksum_calculated = 0x55;
                for (unsigned i = 0; i < count * 5; ++i)
                    diag->checksum_calculated += raw[4 + i];
            }
        }
        // 沿用厂家策略：第一次坏帧先重读；最终尝试必须发送读取结束命令。
        if (result != ESP_OK && result != ESP_ERR_NOT_SUPPORTED && attempt == 0) continue;
        // 这里 ACK 是协议层“读取结束”命令，不是 I2C 每个字节后的硬件 ACK 位。
        esp_err_t ack = command(0xD00002AB);
        diag->ack_attempted = true;
        diag->ack_result = ack;
        diag->int_after_ack = gpio_get_level((gpio_num_t)tp_int_gpio);
        if (ack != ESP_OK) return ack; // 应答失败则整轮失败，不向上层提交此次坐标。
        return result;
    }
    return result;
}

// 第二阶段：安装 INT 下降沿通知，保存分辨率/回调并清空历史触点。
// 不创建后台任务；真正的读取、解析和回调由调用 poll 的任务执行。
esp_err_t cst3530_start(const cst3530_info_t *info, cst3530_callback_t callback, void *user) {
    if (!initialized || reporting) return ESP_ERR_INVALID_STATE;
    if (!info || !callback || !info->resolution_x || !info->resolution_y)
        return ESP_ERR_INVALID_ARG;
    // 不在启动时盲目 ACK，保留启动前已就绪的按下/释放报告。
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    // INVALID_STATE 表示全局 ISR 服务已存在，仅注册本 TP 引脚，不卸载共享服务。
    err = gpio_intr_disable((gpio_num_t)tp_int_gpio);
    if (err != ESP_OK) return err;
    err = gpio_set_intr_type((gpio_num_t)tp_int_gpio, GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) return err;
    (void)take_irq_notification();
    err = gpio_isr_handler_add((gpio_num_t)tp_int_gpio, tp_irq_handler, NULL);
    if (err != ESP_OK) return err;
    err = gpio_intr_enable((gpio_num_t)tp_int_gpio);
    if (err != ESP_OK) {
        gpio_isr_handler_remove((gpio_num_t)tp_int_gpio);
        return err;
    }
    report_width = info->resolution_x;
    report_height = info->resolution_y;
    report_callback = callback;
    callback_user = user;
    memset(active_points, 0, sizeof(active_points));
    failed_frames = 0;
    retry_after_us = 0;
    reporting = true;
    return ESP_OK;
}

// 运行阶段：每次调用最多处理一轮报告；空闲立即返回，不进行周期性盲读。
esp_err_t cst3530_poll(void) {
    if (!reporting) return ESP_ERR_INVALID_STATE;
    int64_t now = esp_timer_get_time();
    if (now < retry_after_us) return ESP_OK; // 只在真实错误后退避，未消费新通知。
    bool notified = take_irq_notification();
    int level = gpio_get_level((gpio_num_t)tp_int_gpio);
    if (!notified && level != 0) return ESP_OK;
    // 实测报告保持低电平直到 ACK。过期通知但当前为高时不读取全零空闲帧。
    // 高电平不是松手：保持现有触点，等待有效释放报告。
    if (level != 0) return ESP_OK;
    // 低电平兜底覆盖启动时已就绪、遗漏边沿和 ACK 后新报告仍待处理的情况。
    cst3530_frame_t frame;
    esp_err_t err = read_touch_frame(&frame);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        failed_frames = 0; // 有效但非坐标报告不计入连续通信/帧损坏次数。
        return err;
    }
    if (err != ESP_OK) {
        retry_after_us = esp_timer_get_time() + 20000; // 错误后退避 20ms，不在此阻塞等待。
        if (failed_frames < 5) ++failed_frames;
        if (failed_frames == 5) {
            // 连续失败达到阈值，取消已有按压；清除 pressed 后不会反复上报同一取消。
            for (unsigned id = 0; id < 16; ++id) {
                if (!active_points[id].pressed) continue;
                active_points[id].pressed = false;
                // 异常取消与正常 UP 分开，上层不得将 CANCEL 当成点击。
                report_callback(CST3530_CANCEL, &active_points[id], callback_user);
            }
        }
        return err;
    }
    failed_frames = 0;
    retry_after_us = 0;
    cst3530_point_t next[16] = {0}; // 建立本次有效报告中的“仍按下”触点集合。
    for (unsigned i = 0; i < frame.count; ++i) {
        if (frame.points[i].pressed) next[frame.points[i].id] = frame.points[i];
    }
    // 有效坐标报告作为当前触点集合；消失或明确释放的 ID 产生 UP。
    for (unsigned id = 0; id < 16; ++id) {
        cst3530_point_t old = active_points[id];
        active_points[id] = next[id];
        // 旧按下/新松开 -> UP；旧松开/新按下 -> DOWN；持续按下且坐标变化 -> MOVE。
        // 只有压力变化不会产生 MOVE，但保存的触点信息仍会更新。
        if (old.pressed && !next[id].pressed) {
            old.pressed = false;
            report_callback(CST3530_UP, &old, callback_user);
        } else if (next[id].pressed && !old.pressed) {
            report_callback(CST3530_DOWN, &active_points[id], callback_user);
        } else if (next[id].pressed && (next[id].x != old.x || next[id].y != old.y)) {
            report_callback(CST3530_MOVE, &active_points[id], callback_user);
        }
    }
    return ESP_OK;
}

// 启动流程中读取运行信息：发送正常模式命令 -> 读 50 字节 -> 检查标识并提取字段。
// 建议在 start 前调用；这些模式命令不是普通触摸读帧，不应与 poll 并发执行。
esp_err_t cst3530_read_info(cst3530_info_t *info) {
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    if (!initialized) return ESP_ERR_INVALID_STATE;
    // 参考厂家 hyn_cst66xx.c 的正常模式和 updata_tpinfo（名称含 updata，
    // 这里实际为读取运行信息，不是升级固件）。无 A0 Boot 命令、无 Flash 写入。
    const uint32_t normal_commands[] = {0xD0000000, 0xD0000C00, 0xD0000100};
    const uint8_t read_command[] = {0xD0, 0x03, 0x00, 0x00};
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 0; attempt < 4; ++attempt) { // 最多 4 次，失败间执行厂家恢复序列。
        uint8_t data[50] = {0};
        err = ESP_OK;
        for (unsigned i = 0; i < sizeof(normal_commands)/sizeof(normal_commands[0]); ++i) {
            err = command(normal_commands[i]);
            if (err != ESP_OK) break;
        }
        if (err == ESP_OK)
            err = bsp_i2c_transfer(CST3530_I2C_ADDRESS, read_command, sizeof(read_command),
                                   data, sizeof(data));
        if (err == ESP_OK) {
            // 厂家信息读取只检查 CACA 标识，此处再检查分辨率非零。
            // 不把信息标识检查冒充触摸帧校验和检查。
            if (data[2] != 0xCA || data[3] != 0xCA ||
                read_le16(data + 28) == 0 || read_le16(data + 30) == 0) {
                err = ESP_ERR_INVALID_RESPONSE;
            } else {
                // 保留完整原始信息，字段偏移沿用厂家协议，便于后续追踪版本差异。
                memcpy(info->raw, data, sizeof(data));
                info->chip_type = read_le32(data);
                info->firmware_version = read_le32(data + 32);
                info->project_id = read_le32(data + 36);
                info->resolution_x = read_le16(data + 28);
                info->resolution_y = read_le16(data + 30);
                info->key_count = data[27];
                info->tx_channels = data[48];
                info->rx_channels = data[49];
                return ESP_OK;
            }
        }
        if (attempt < 3) {
            wait_ms(1);
            esp_err_t recovery = command(0xD0000400); // 厂家信息读取重试流程。
            if (recovery != ESP_OK) return recovery;
            wait_ms(1);
        }
    }
    return err;
}
