#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    while (1) {
        printf("Hello_world\n");
        vTaskDelay(pdMS_TO_TICKS(1000)); // 每 1 秒打印一次
    }
}