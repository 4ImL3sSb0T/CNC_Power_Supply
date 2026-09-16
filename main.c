#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include "driver/led/led.h"

void led_task(void *pvParameters) {
    (void) pvParameters; // Unused parameter

    led_init(); // Initialize the LED

    while (1) {
        LED_TOGGLE(); // Toggle the LED state
        vTaskDelay(pdMS_TO_TICKS(500)); // Delay for 500 milliseconds
    }
}

int main()
{
    stdio_init_all();

    xTaskCreate(led_task, "LED Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);

    printf("Starting FreeRTOS scheduler...\n");
    vTaskStartScheduler();  
}
