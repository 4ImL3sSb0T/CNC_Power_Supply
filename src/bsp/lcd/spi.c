/**
 ****************************************************************************************************
 * @file        spi.c
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

#include "bsp/lcd/spi.h"


/**
 * @brief       初始化SPI
 * @param       无
 * @retval      无
 */
void spi1_init(void)
{
    gpio_set_pulls(SPI_CLK_GPIO_PIN, true, false);                      /* 设置CLK引脚上拉 */
    gpio_set_pulls(SPI_MOSI_GPIO_PIN, true, false);                     /* 设置MOSI引脚上拉 */
    gpio_set_function(SPI_CLK_GPIO_PIN, GPIO_FUNC_SPI);                 /* 设置SCLK引脚为SPI功能 */
    gpio_set_function(SPI_MOSI_GPIO_PIN, GPIO_FUNC_SPI);                /* 设置MOSI引脚为SPI功能 */
    gpio_set_function(SPI_MISO_GPIO_PIN, GPIO_FUNC_SPI);                /* 设置MISO引脚为SPI功能 */

    /* 初始化SPI总线 */
    spi_init(SPI_PORT, SPI_BAUD_RATE);                                  /* SPI1初始化并设置波特率 */

    /* 配置SPI总线参数 */
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST); /* 8位数据，CPOL=0, CPHA=0, MSB优先 */
}
