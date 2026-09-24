#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "es8311.h"

#define TAG "AUDIO"
#define I2C_PORT I2C_NUM_0
#define I2C_SDA 7
#define I2C_SCL 8
#define AMP_POWER_GPIO 53
#define I2S_PORT I2S_NUM_0
#define I2S_MCLK 13
#define I2S_BCLK 12
#define I2S_LRCK 10
#define I2S_DOUT 9
#define SAMPLE_RATE 16000
#define TONE_FRAME 64
#define NOTE_AMPLITUDE 8000
#define MELODY_VOLUME 90

typedef struct { float freq; uint16_t duration_ms; } note_t;
static const note_t melody[] = {
    {659.25f,350},{622.25f,350},{659.25f,350},{622.25f,350},
    {659.25f,350},{493.88f,350},{587.33f,350},{523.25f,350},
    {440.00f,700},{261.63f,350},{329.63f,350},{440.00f,350},
    {493.88f,700},{329.63f,350},{415.30f,350},{493.88f,350},
    {523.25f,700},{329.63f,350},{659.25f,350},{622.25f,350},
    {659.25f,350},{622.25f,350},{659.25f,350},{493.88f,350},
    {587.33f,350},{523.25f,350},{440.00f,900},
};

static esp_err_t init_i2c(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER, .sda_io_num = I2C_SDA, .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_PORT, &cfg), TAG, "I2C config failed");
    return i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0);
}

static esp_err_t init_i2s(void)
{
    i2s_config_t cfg = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8, .dma_buf_len = 64,
        .use_apll = false, .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
    };
    i2s_pin_config_t pins = {
        .mck_io_num = I2S_MCLK, .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRCK, .data_out_num = I2S_DOUT, .data_in_num = I2S_PIN_NO_CHANGE,
    };
    ESP_RETURN_ON_ERROR(i2s_driver_install(I2S_PORT, &cfg, 0, NULL), TAG, "I2S install failed");
    ESP_RETURN_ON_ERROR(i2s_set_pin(I2S_PORT, &pins), TAG, "I2S pins failed");
    return i2s_zero_dma_buffer(I2S_PORT);
}

static esp_err_t init_codec(void)
{
    es8311_handle_t codec = es8311_create(I2C_PORT, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(codec != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 allocation failed");
    const es8311_clock_config_t clk = {
        .mclk_inverted = false, .sclk_inverted = false,
        .mclk_from_mclk_pin = true, .mclk_frequency = SAMPLE_RATE * 256,
        .sample_frequency = SAMPLE_RATE,
    };
    esp_err_t err = es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (err == ESP_OK) err = es8311_sample_frequency_config(codec, clk.mclk_frequency, clk.sample_frequency);
    if (err == ESP_OK) err = es8311_microphone_config(codec, false);
    if (err == ESP_OK) err = es8311_voice_volume_set(codec, MELODY_VOLUME, NULL);
    es8311_delete(codec);
    return err;
}

static void play_note(const note_t *note)
{
    const uint32_t total = (uint64_t)SAMPLE_RATE * note->duration_ms / 1000;
    uint32_t done = 0;
    float phase = 0.0f;
    const float step = note->freq * 2.0f * (float)M_PI / SAMPLE_RATE;
    int16_t frame[TONE_FRAME * 2];
    while (done < total) {
        uint32_t n = (total - done < TONE_FRAME) ? total - done : TONE_FRAME;
        for (uint32_t i = 0; i < n; ++i) {
            float p = (float)(done + i) / total;
            float env = (p < 0.15f) ? p / 0.15f : 1.0f - 0.6f * (p - 0.15f) / 0.85f;
            int16_t sample = (int16_t)(sinf(phase) * NOTE_AMPLITUDE * env);
            frame[2*i] = frame[2*i+1] = sample;
            phase += step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        size_t written = 0;
        ESP_ERROR_CHECK(i2s_write(I2S_PORT, frame, n * 2 * sizeof(int16_t), &written, portMAX_DELAY));
        ESP_ERROR_CHECK(written == n * 2 * sizeof(int16_t) ? ESP_OK : ESP_FAIL);
        done += n;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(gpio_set_level(AMP_POWER_GPIO, 0));
    ESP_ERROR_CHECK(gpio_set_direction(AMP_POWER_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(init_i2c());
    ESP_ERROR_CHECK(init_i2s());
    ESP_ERROR_CHECK(init_codec());
    ESP_ERROR_CHECK(gpio_set_level(AMP_POWER_GPIO, 1));
    ESP_LOGI(TAG, "ES8311 ready, playing Fuer Elise");
    while (true) {
        for (size_t i = 0; i < sizeof(melody)/sizeof(melody[0]); ++i) {
            play_note(&melody[i]);
            vTaskDelay(pdMS_TO_TICKS(60));
        }
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
}
