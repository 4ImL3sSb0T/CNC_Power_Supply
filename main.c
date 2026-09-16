#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "driver/imu/imu.h"
#include "driver/led/led.h"

// ---------------- CPU 占用率统计 ----------------
// 基于 FreeRTOS 运行时间统计（FreeRTOSConfig.h 里 configGENERATE_RUN_TIME_STATS）。
// ulTaskGetRunTimeCounter() 只累计任务“真正在运行”的时间，阻塞等待不计入，
// 所以 IMU 那一栏就是读取方式本身的 CPU 开销：
//   IMU %   —— IMU 任务占用（单核百分比，100% = 占满一个核）
//   空闲 %  —— 各核 idle 任务的运行时间占比
static volatile u32 s_read_count = 0;   // 成功读取次数（同时看吞吐有没有被拖慢）
static TaskHandle_t s_imu_task = NULL;

void sysmon_task(void *pvParameters) {
    (void) pvParameters;

    u32 prev_imu = ulTaskGetRunTimeCounter(s_imu_task);
    u32 prev_idle0 = ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandleForCore(0));
    u32 prev_idle1 = ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandleForCore(1));
    u32 prev_reads = s_read_count;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // 计数单位是 µs：1 秒内跑满 1000000 µs 即占满一个核
        u32 imu_now = ulTaskGetRunTimeCounter(s_imu_task);
        u32 idle0_now = ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandleForCore(0));
        u32 idle1_now = ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandleForCore(1));
        u32 reads_now = s_read_count;

        printf("IMU %5.1f%%/核 | 空闲 core0 %5.1f%% core1 %5.1f%% | %u 次/s\n",
               (imu_now - prev_imu) / 10000.0f,
               (idle0_now - prev_idle0) / 10000.0f,
               (idle1_now - prev_idle1) / 10000.0f,
               (unsigned)(reads_now - prev_reads));

        prev_imu = imu_now;
        prev_idle0 = idle0_now;
        prev_idle1 = idle1_now;
        prev_reads = reads_now;
    }
}

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

    vec3f accel;

    imu_probe_version();

    while (1) {
        // 满速读取、不做任何打印：串口输出本身也占 CPU，会干扰占用率测量
        if (imu_get_accel(&accel) == EXIT_OK) {
            s_read_count++;
        }
    }
}

int main()
{
    stdio_init_all();
    imu_init(i2c0, 16, 17); // Initialize the IMU

#if IMU_USE_DMA
    printf("IMU 读取模式: DMA (driver/i2c/i2c_dma)\n");
#else
    printf("IMU 读取模式: SDK 阻塞 (i2c_*_blocking)\n");
#endif

    xTaskCreate(led_task, "LED Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);
    xTaskCreate(imu_test_task, "IMU Test Task", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, &s_imu_task);
    xTaskCreate(sysmon_task, "SysMon Task", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, NULL);

    printf("Starting FreeRTOS scheduler...\n");
    vTaskStartScheduler();
}
