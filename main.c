#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "driver/imu/imu.h"
#include "driver/led/led.h"

void led_task(void *pvParameters) {
    (void) pvParameters; // Unused parameter

    led_init(); // Initialize the LED

    while (1) {
        LED_TOGGLE(); // Toggle the LED state
        vTaskDelay(pdMS_TO_TICKS(500)); // Delay for 500 milliseconds
    }
}

void imu_test_task(void *pvParameters) {
    (void) pvParameters; // Unused parameter

    vec3f accel, gyro, mag;

    // 版本号探测必须在任务上下文里做（DMA 传输会阻塞等待中断通知）
    imu_probe_version();

    while (1) {
        if (imu_get_accel(&accel) == EXIT_OK) {
            printf("Accel: X=%.2f, Y=%.2f, Z=%.2f\n", accel.x, accel.y, accel.z);
        } else {
            printf("Failed to read accelerometer data\n");
        }

        // if (imu_get_gyro(&gyro) == EXIT_OK) {
        //     printf("Gyro: X=%.2f, Y=%.2f, Z=%.2f\n", gyro.x, gyro.y, gyro.z);
        // } else {
        //     printf("Failed to read gyroscope data\n");
        // }

        // if (imu_get_mag(&mag) == EXIT_OK) {
        //     printf("Mag: X=%.2f, Y=%.2f, Z=%.2f\n", mag.x, mag.y, mag.z);
        // } else {
        //     printf("Failed to read magnetometer data\n");
        // }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

int main()
{
    stdio_init_all();
    imu_init(i2c0, 16, 17); // Initialize the IMU

    xTaskCreate(led_task, "LED Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);
    xTaskCreate(imu_test_task, "IMU Test Task", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, NULL);

    printf("Starting FreeRTOS scheduler...\n");
    vTaskStartScheduler();  
}
