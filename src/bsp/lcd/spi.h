/**
 ****************************************************************************************************
 * @file        spi.h
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-03
 * @brief       SPI驱动代码
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

#ifndef __SPI_H
#define __SPI_H

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"


/* 引脚定义 */
#define SPI_PORT            spi1
#define SPI_CLK_GPIO_PIN    10          /* SPI1_CLK */
#define SPI_MOSI_GPIO_PIN   11          /* SPI1_MOSI */
#define SPI_MISO_GPIO_PIN   12          /* SPI1_MISO */
#define SPI_BAUD_RATE       62500000    /* SPI波特率，ST7789 写周期常见上限 */

/* 函数声明 */
void spi1_init(void);                   /* 初始化SPI1 */

#endif
