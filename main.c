#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

int main()
{
    stdio_init_all();

    printf("Starting FreeRTOS scheduler...\n");
    vTaskStartScheduler();  
}
