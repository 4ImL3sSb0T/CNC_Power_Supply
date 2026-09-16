/**
 ****************************************************************************************************
 * @file        led.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-03
 * @brief       LED驱动代码
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 RP2350A 最小系统板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 * 
 ****************************************************************************************************
 */

#ifndef __LED_H_
#define __LED_H_

#include "pico/stdlib.h"
#include <stdio.h>


/* 引脚定义 */
#define LED_GPIO_PIN    3                       /* LED连接的GPIO端口(正点原子RP2350A: IO3) */

/* 引脚的输出的电平状态 */
enum GPIO_OUTPUT_STATE
{
    PIN_RESET,
    PIN_SET
};

/* LED端口定义 */
#define LED(x)          do { x ?                                \
                             gpio_put(LED_GPIO_PIN, PIN_SET) :  \
                             gpio_put(LED_GPIO_PIN, PIN_RESET); \
                        } while(0)  /* LED翻转 */

/* LED取反定义 */
#define LED_TOGGLE()    do { gpio_put(LED_GPIO_PIN, !gpio_get(LED_GPIO_PIN)); } while(0)  /* LED翻转 */

/* 函数声明*/
void led_init(void);    /* 初始化LED */

#endif