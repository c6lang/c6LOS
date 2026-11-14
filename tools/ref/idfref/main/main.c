#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    int n;
    for (n = 0; n < 16; n++) {
        printf("SPI %02x=%08x\n", n * 4,
               *(volatile unsigned *)(0x60002000u + n * 4));
    }
    for (n = 0; n < 8; n++) {
        printf("MMU %03x=%08x\n", n,
               *(volatile unsigned *)(0x600C5000u + n * 4));
    }
    while (1) { vTaskDelay(pdMS_TO_TICKS(2000)); }
}
