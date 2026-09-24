#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "es7210.h"

#define TAG "MIC_RECORD"
#define I2C_PORT I2C_NUM_0
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define FRAME_LENGTH_MS 30
#define FRAME_SAMPLES (FRAME_LENGTH_MS * SAMPLE_RATE / 1000)

static int16_t frame[FRAME_SAMPLES];

static esp_err_t init_i2c(void)
{
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_7,
        .scl_io_num = GPIO_NUM_8,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_PORT, &config), TAG, "I2C config failed");
    return i2c_driver_install(I2C_PORT, config.mode, 0, 0, 0);
}

static void scan_i2c_bus(void)
{
    puts("I2C scan start");
    unsigned found = 0;
    for (uint8_t addr = 1; addr < 127; ++addr) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        if (!cmd) break;
        esp_err_t err = i2c_master_start(cmd);
        if (err == ESP_OK) err = i2c_master_write_byte(cmd, addr << 1, true);
        if (err == ESP_OK) err = i2c_master_stop(cmd);
        if (err == ESP_OK) err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            printf("I2C device: 0x%02X\n", addr);
            ++found;
        }
    }
    printf("I2C scan done, devices=%u\n", found);
}

static esp_err_t init_i2s(void)
{
    const i2s_config_t config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        // Match the Arduino example's SDOUT1 left-slot configuration.
        .channel_format = I2S_CHANNEL_FMT_ALL_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
    };
    const i2s_pin_config_t pins = {
        .mck_io_num = GPIO_NUM_13,
        .bck_io_num = GPIO_NUM_12,
        .ws_io_num = GPIO_NUM_10,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = GPIO_NUM_11,
    };
    ESP_RETURN_ON_ERROR(i2s_driver_install(I2S_PORT, &config, 0, NULL), TAG, "I2S install failed");
    esp_err_t err = i2s_set_pin(I2S_PORT, &pins);
    if (err == ESP_OK) err = i2s_zero_dma_buffer(I2S_PORT);
    if (err != ESP_OK) i2s_driver_uninstall(I2S_PORT);
    return err;
}

static esp_err_t init_codec(void)
{
    audio_hal_codec_config_t config = {
        .adc_input = AUDIO_HAL_ADC_INPUT_ALL,
        .codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE,
        .i2s_iface = {
            .mode = AUDIO_HAL_MODE_SLAVE,
            .fmt = AUDIO_HAL_I2S_NORMAL,
            .samples = AUDIO_HAL_16K_SAMPLES,
            .bits = AUDIO_HAL_BIT_LENGTH_16BITS,
        },
    };
    const es7210_input_mics_t mics = ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2;
    ESP_RETURN_ON_ERROR(es7210_adc_init(I2C_PORT, &config), TAG, "ES7210 init failed");
    ESP_RETURN_ON_ERROR(es7210_adc_config_i2s(config.codec_mode, &config.i2s_iface), TAG, "ES7210 I2S config failed");
    // GPIO11 is connected to SDOUT1 (MIC1/MIC2); MIC3/MIC4 are unused.
    ESP_RETURN_ON_ERROR(es7210_mic_select(mics), TAG, "Mic selection failed");
    ESP_RETURN_ON_ERROR(es7210_adc_set_gain(mics, GAIN_24DB), TAG, "Mic gain failed");
    return es7210_adc_ctrl_state(config.codec_mode, AUDIO_HAL_CTRL_START);
}

static void print_frame(void)
{
    int32_t peak = 0;
    int64_t sum_sq = 0;
    for (int i = 0; i < FRAME_SAMPLES; ++i) {
        int32_t value = frame[i];
        int32_t magnitude = value < 0 ? -value : value;
        if (magnitude > peak) peak = magnitude;
        sum_sq += (int64_t)value * value;
    }
    int32_t rms = (int32_t)sqrtf((float)sum_sq / FRAME_SAMPLES);
    char status[256];
    int length = snprintf(status, sizeof(status), "%llu peak=%ld rms=%ld samples=",
                          (unsigned long long)(esp_timer_get_time() / 1000), (long)peak, (long)rms);
    for (int i = 0; i < 16 && length >= 0 && length < sizeof(status); ++i) {
        length += snprintf(status + length, sizeof(status) - length, "%d,",
                           frame[i * (FRAME_SAMPLES / 16)]);
    }
    puts(status);
}

void app_main(void)
{
    // Keep the speaker amplifier off while testing the microphones.
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_53, 0));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_53, GPIO_MODE_OUTPUT));
    esp_err_t err = init_i2c();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return;
    }
    scan_i2c_bus();
    err = init_i2s();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        i2c_driver_delete(I2C_PORT);
        return;
    }
    err = init_codec();
    printf("ES7210 init result: %s\n", esp_err_to_name(err));
    int reg = es7210_read_reg(ES7210_RESET_REG00);
    if (reg >= 0) printf("ES7210 REG00: 0x%02X\n", reg);
    if (err != ESP_OK || reg < 0) {
        ESP_LOGE(TAG, "ES7210 init failed; capture stopped");
        i2s_driver_uninstall(I2S_PORT);
        i2c_driver_delete(I2C_PORT);
        return;
    }
    puts("ES7210 ready; printing mic PCM frames");
    while (true) {
        size_t bytes_read = 0;
        err = i2s_read(I2S_PORT, frame, sizeof(frame), &bytes_read, pdMS_TO_TICKS(1000));
        if (err != ESP_OK || bytes_read != sizeof(frame)) {
            ESP_LOGE(TAG, "I2S read failed: %s, bytes: %u", esp_err_to_name(err), (unsigned)bytes_read);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        print_frame();
    }
}
