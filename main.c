/**
 * @file        main.c
 * @brief       数控电源v1 验证板(PCB) 测试台固件入口
 *
 * 任务：
 *   pcb_test_task  测试序列状态机（唯一操作 SC8701 控制脚的任务）
 *   ui_task        屏幕 20Hz 刷新（唯一 HAGL 使用者）
 *   key_task       MultiButton 5ms 打拍
 *   sysmon_task    CPU 占用率统计（串口，1s）
 *   led_task       心跳灯 500ms
 *
 * 上电第一件事是 pcb_ctrl_init()：SC8701 的 /CE 悬空即上电使能、
 * IPWM 悬空芯片不能正常工作，必须最先置安全态（CE# 关断、IPWM 0%）。
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include "config/board_config.h"
#include "bsp/power/pcb_ctrl.h"
#include "bsp/input/key.h"
#include "bsp/lcd/spi.h"
#include "bsp/lcd/lcd.h"
#include "hagl.h"
#include "driver/imu/imu.h"
#include "driver/led/led.h"
#include "app/pcb_test/pcb_test.h"
#include "app/ui/ui.h"

// ---------------- CPU 占用率统计 ----------------
// 基于 FreeRTOS 运行时间统计（FreeRTOSConfig.h 里 configGENERATE_RUN_TIME_STATS）。
// ulTaskGetRunTimeCounter() 只累计任务"真正在运行"的时间，阻塞等待不计入：
//   TEST %  —— 测试任务占用（单核百分比，100% = 占满一个核）
//   空闲 %  —— 各核 idle 任务的运行时间占比
static TaskHandle_t s_test_task = NULL;

static void sysmon_task(void *pvParameters)
{
    (void) pvParameters;

    u32 prev_test = ulTaskGetRunTimeCounter(s_test_task);
    u32 prev_idle0 = ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandleForCore(0));
    u32 prev_idle1 = ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandleForCore(1));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // 计数单位是 µs：1 秒内跑满 1000000 µs 即占满一个核
        u32 test_now = ulTaskGetRunTimeCounter(s_test_task);
        u32 idle0_now = ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandleForCore(0));
        u32 idle1_now = ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandleForCore(1));

        printf("TEST %5.1f%%/核 | 空闲 core0 %5.1f%% core1 %5.1f%%\n",
               (test_now - prev_test) / 10000.0f,
               (idle0_now - prev_idle0) / 10000.0f,
               (idle1_now - prev_idle1) / 10000.0f);

        prev_test = test_now;
        prev_idle0 = idle0_now;
        prev_idle1 = idle1_now;
    }
}

static void led_task(void *pvParameters)
{
    (void) pvParameters;

    led_init();

    while (1) {
        LED_TOGGLE();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void key_task(void *pvParameters)
{
    (void) pvParameters;

    while (1) {
        key_tick();
        vTaskDelay(pdMS_TO_TICKS(TICKS_INTERVAL));
    }
}

int main()
{
    hagl_backend_t *display;

    /* 安全态最先：控制脚未配置前 SC8701 处于不可控状态 */
    pcb_ctrl_init();

    stdio_init_all();
    led_init();
    spi1_init();    /* 必须显式先调，lcd_init 内部不会调它 */
    lcd_init();
    display = hagl_init();
    key_init();
    imu_init(i2c0, IMU_SDA_GPIO, IMU_SCL_GPIO);
    pcb_test_init();
    pcb_test_bind_keys();

    printf("数控电源v1 测试台启动 | H1: 1=AGND 2=IPWM 3=PWM 4=PG 5=CE# 6=3V3\n");
    printf("按键: 单击=跑测试 双击=步进电压设定 长按=急停\n");

    xTaskCreate(pcb_test_task, "Test Task", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, &s_test_task);
    xTaskCreate(ui_task, "UI Task", configMINIMAL_STACK_SIZE * 2, display, tskIDLE_PRIORITY + 1, NULL);
    xTaskCreate(key_task, "Key Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);
    xTaskCreate(sysmon_task, "SysMon Task", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, NULL);
    xTaskCreate(led_task, "LED Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);

    printf("Starting FreeRTOS scheduler...\n");
    vTaskStartScheduler();
}
