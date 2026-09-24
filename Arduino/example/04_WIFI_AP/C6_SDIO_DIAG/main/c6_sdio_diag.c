#include <inttypes.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SDIO_CMD_GPIO       GPIO_NUM_18
#define SDIO_CLK_GPIO       GPIO_NUM_19
#define SAMPLE_PERIOD_MS    100
#define PCNT_LOW_LIMIT      (-1)
#define PCNT_HIGH_LIMIT     32767

static const char *TAG = "C6_SDIO_DIAG";

typedef struct {
    gpio_num_t gpio;
    const char *name;
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t channel;
} signal_counter_t;

static void start_rising_edge_counter(signal_counter_t *counter)
{
    pcnt_unit_config_t unit_config = {
        .low_limit = PCNT_LOW_LIMIT,
        .high_limit = PCNT_HIGH_LIMIT,
        .flags.accum_count = 1,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &counter->unit));

    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 100,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(counter->unit, &filter_config));

    pcnt_chan_config_t channel_config = {
        .edge_gpio_num = counter->gpio,
        .level_gpio_num = -1,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(counter->unit, &channel_config, &counter->channel));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(counter->channel,
                                                  PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                  PCNT_CHANNEL_EDGE_ACTION_HOLD));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(counter->channel,
                                                   PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                   PCNT_CHANNEL_LEVEL_ACTION_KEEP));
    ESP_ERROR_CHECK(pcnt_unit_enable(counter->unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(counter->unit));
    ESP_ERROR_CHECK(pcnt_unit_start(counter->unit));
}

void app_main(void)
{
    signal_counter_t clk = {
        .gpio = SDIO_CLK_GPIO,
        .name = "CLK",
    };
    signal_counter_t cmd = {
        .gpio = SDIO_CMD_GPIO,
        .name = "CMD",
    };

    /* Keep both SDIO pins strictly as inputs. The PCNT driver adds the input
       path used by the hardware counters; this firmware never drives them. */
    gpio_config_t input_config = {
        .pin_bit_mask = (1ULL << SDIO_CLK_GPIO) | (1ULL << SDIO_CMD_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_config));

    start_rising_edge_counter(&clk);
    start_rising_edge_counter(&cmd);

    ESP_LOGI(TAG, "ESP32-C6 P4-to-C6 SDIO physical-link diagnostic");
    ESP_LOGI(TAG, "Monitoring C6 GPIO19=CLK and GPIO18=CMD at 115200 baud");
    ESP_LOGI(TAG, "Run the P4 WiFi sketch now; do not ground C6 GPIO9");
    printf("DIAG_READY CLK=%d CMD=%d\n", gpio_get_level(SDIO_CLK_GPIO),
           gpio_get_level(SDIO_CMD_GPIO));

    int previous_clk_count = 0;
    int previous_cmd_count = 0;
    int quiet_reports = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));

        int clk_count = 0;
        int cmd_count = 0;
        ESP_ERROR_CHECK(pcnt_unit_get_count(clk.unit, &clk_count));
        ESP_ERROR_CHECK(pcnt_unit_get_count(cmd.unit, &cmd_count));

        int clk_delta = clk_count - previous_clk_count;
        int cmd_delta = cmd_count - previous_cmd_count;
        previous_clk_count = clk_count;
        previous_cmd_count = cmd_count;

        if (clk_delta != 0 || cmd_delta != 0) {
            printf("SDIO_ACTIVITY t=%" PRId64 "ms CLK_rise=+%d(total=%d) "
                   "CMD_rise=+%d(total=%d) levels CLK=%d CMD=%d\n",
                   esp_timer_get_time() / 1000,
                   clk_delta, clk_count, cmd_delta, cmd_count,
                   gpio_get_level(SDIO_CLK_GPIO),
                   gpio_get_level(SDIO_CMD_GPIO));
            quiet_reports = 0;
        } else if (++quiet_reports >= 10) {
            printf("NO_ACTIVITY t=%" PRId64 "ms totals CLK=%d CMD=%d "
                   "levels CLK=%d CMD=%d\n",
                   esp_timer_get_time() / 1000,
                   clk_count, cmd_count,
                   gpio_get_level(SDIO_CLK_GPIO),
                   gpio_get_level(SDIO_CMD_GPIO));
            quiet_reports = 0;
        }
        fflush(stdout);
    }
}
