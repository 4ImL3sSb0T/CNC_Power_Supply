/**
 * @file        lcd_bus.c
 * @brief       ST7789 面板协议层：DC 边带 + 命令 / 参数 / 窗口 / 像素的编排
 *
 * 这里不再碰任何 SPI 寄存器、DMA 通道或中断——CS 窗口、字节流、超时和异常恢复
 * 都是 bsp_spi 的职责。本文件只回答"该发哪些字节、DC 什么时候翻"。
 */

#include "driver/lcd/lcd_priv.h"

/* DC 是协议语义（这一包是命令还是参数），归 driver：active = 高电平 = 数据。 */
static inline exit_code_t lcd_dc(bool data)
{
    return bsp_gpio_set_active(BSP_GPIO_LCD_DC, data);
}

exit_code_t lcd_txn_begin(void)
{
    return bsp_spi_cs_assert(BSP_SPI_DEV_LCD, LCD_TXN_TIMEOUT_MS);
}

exit_code_t lcd_txn_end(exit_code_t prior)
{
    const exit_code_t rc = bsp_spi_cs_release(BSP_SPI_DEV_LCD);

    return (prior != EXIT_OK) ? prior : rc;
}

exit_code_t lcd_cmd_locked(uint8_t cmd)
{
    exit_code_t rc = lcd_dc(false);

    if (rc != EXIT_OK)
    {
        return rc;
    }
    return bsp_spi_transfer(BSP_SPI_DEV_LCD, &cmd, NULL, 1, LCD_TXN_TIMEOUT_MS);
}

exit_code_t lcd_data_locked(const uint8_t *data, size_t len)
{
    exit_code_t rc;

    /* 初始化表里有只带命令、不带参数的表项（databytes == 0），按空操作处理。 */
    if (len == 0)
    {
        return EXIT_OK;
    }
    if (data == NULL)
    {
        return EXIT_INVALID_PARAM;
    }

    rc = lcd_dc(true);
    if (rc != EXIT_OK)
    {
        return rc;
    }
    return bsp_spi_transfer(BSP_SPI_DEV_LCD, data, NULL, len, LCD_TXN_TIMEOUT_MS);
}

exit_code_t lcd_window_locked(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t buf[4];
    uint16_t xs = (uint16_t)(x0 + lcd_self.colstart);
    uint16_t xe = (uint16_t)(x1 + lcd_self.colstart);
    uint16_t ys = (uint16_t)(y0 + lcd_self.rowstart);
    uint16_t ye = (uint16_t)(y1 + lcd_self.rowstart);
    exit_code_t rc;

    buf[0] = (uint8_t)(xs >> 8);
    buf[1] = (uint8_t)xs;
    buf[2] = (uint8_t)(xe >> 8);
    buf[3] = (uint8_t)xe;
    rc = lcd_cmd_locked(0x2A);
    if (rc == EXIT_OK)
    {
        rc = lcd_data_locked(buf, 4);
    }

    buf[0] = (uint8_t)(ys >> 8);
    buf[1] = (uint8_t)ys;
    buf[2] = (uint8_t)(ye >> 8);
    buf[3] = (uint8_t)ye;
    if (rc == EXIT_OK)
    {
        rc = lcd_cmd_locked(0x2B);
    }
    if (rc == EXIT_OK)
    {
        rc = lcd_data_locked(buf, 4);
    }

    /* 0x2C 之后跟的才是像素流。 */
    if (rc == EXIT_OK)
    {
        rc = lcd_cmd_locked(0x2C);
    }

    return rc;
}

exit_code_t lcd_pixels_locked(const uint16_t *pixels, size_t count)
{
    exit_code_t rc;

    if (count == 0)
    {
        return EXIT_OK;
    }
    if (pixels == NULL)
    {
        return EXIT_INVALID_PARAM;
    }

    rc = lcd_dc(true);
    if (rc != EXIT_OK)
    {
        return rc;
    }
    return bsp_spi_transfer16(BSP_SPI_DEV_LCD, pixels, count, LCD_TXN_TIMEOUT_MS);
}

exit_code_t lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    exit_code_t rc = lcd_txn_begin();

    if (rc != EXIT_OK)
    {
        return rc;
    }

    return lcd_txn_end(lcd_window_locked(x0, y0, x1, y1));
}

exit_code_t lcd_write_cmd(uint8_t cmd)
{
    exit_code_t rc = lcd_txn_begin();

    if (rc != EXIT_OK)
    {
        return rc;
    }

    return lcd_txn_end(lcd_cmd_locked(cmd));
}

exit_code_t lcd_write_data(const uint8_t *data, size_t len)
{
    exit_code_t rc = lcd_txn_begin();

    if (rc != EXIT_OK)
    {
        return rc;
    }

    return lcd_txn_end(lcd_data_locked(data, len));
}
