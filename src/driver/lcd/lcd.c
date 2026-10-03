/**
 * @file        lcd.c
 * @brief       ST7789 1.14" 面板：初始化、方向、窗口、单缓冲 flush
 *
 * Derived from ALIENTEK RP2350A LCD example.
 *
 * 与原例子的差别：SPI/DMA 寄存器操作、DMA 中断和 GPIO 直控都已移除，分别换成
 * bsp_spi 和 bsp_gpio。flush 从"启动 DMA 就返回"改成阻塞到发完——bsp_spi 只有
 * 同步传输接口，这也顺带干掉了原来的 fb_lock / is_busy / wait_idle 三件套。
 */

#include "driver/lcd/lcd_priv.h"

#include "FreeRTOS.h"
#include "pico/stdlib.h"
#include "task.h"

uint16_t lcd_buf[LCD_PIXEL_MAX] __attribute__((aligned(4)));
lcd_obj_t lcd_self;

/* 240x135 可见区嵌在 ST7789 240x320 GRAM 里。横屏 MADCTL 对应原 scan_dir=6。 */
#define ST7789_MADCTL_MY        0x80
#define ST7789_MADCTL_MV        0x20

typedef struct
{
    uint16_t width;
    uint16_t height;
    uint16_t colstart;
    uint16_t rowstart;
    uint8_t  madctl;
} lcd_orient_t;

static const lcd_orient_t lcd_orient[2] = {
    { 135, 240, 52, 40, 0x00 },
    { 240, 135, 40, 52, ST7789_MADCTL_MY | ST7789_MADCTL_MV },
};

typedef struct
{
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes;      /* bit7 = 发送后延时；0xFF = 结束 */
} lcd_init_cmd_t;

/* lcd_init 允许在调度器启动前调用，那时 vTaskDelay 不可用。 */
static void lcd_delay_ms(uint32_t ms)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
    {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    else
    {
        sleep_ms(ms);
    }
}

exit_code_t lcd_display_dir(uint8_t dir)
{
    const lcd_orient_t *o = &lcd_orient[dir ? 1 : 0];
    uint8_t madctl = o->madctl;
    exit_code_t rc;

    lcd_self.dir = dir ? 1 : 0;
    lcd_self.width = o->width;
    lcd_self.height = o->height;
    lcd_self.colstart = o->colstart;
    lcd_self.rowstart = o->rowstart;

    rc = lcd_write_cmd(0x36);
    if (rc == EXIT_OK)
    {
        rc = lcd_write_data(&madctl, 1);
    }
    if (rc != EXIT_OK)
    {
        return rc;
    }

    return lcd_set_window(0, 0, (uint16_t)(lcd_self.width - 1), (uint16_t)(lcd_self.height - 1));
}

exit_code_t lcd_flush_rect(uint16_t x, uint16_t y, uint16_t xend, uint16_t yend)
{
    uint16_t fb_w = lcd_self.width;
    uint16_t rect_w;
    uint16_t row;
    exit_code_t rc;

    if (fb_w == 0 || lcd_self.height == 0)
    {
        return EXIT_NOT_INITIALIZED;
    }
    /* 整个矩形都在可见区外：什么都不用做，和原实现的宽松语义保持一致。 */
    if (x >= fb_w || y >= lcd_self.height)
    {
        return EXIT_OK;
    }
    if (xend >= fb_w)
    {
        xend = (uint16_t)(fb_w - 1);
    }
    if (yend >= lcd_self.height)
    {
        yend = (uint16_t)(lcd_self.height - 1);
    }
    if (x > xend || y > yend)
    {
        return EXIT_INVALID_PARAM;
    }

    rect_w = (uint16_t)(xend - x + 1);

    rc = lcd_txn_begin();
    if (rc != EXIT_OK)
    {
        return rc;
    }

    rc = lcd_window_locked(x, y, xend, yend);

    if (rc == EXIT_OK && x == 0 && xend == (uint16_t)(fb_w - 1))
    {
        /* 占满整行宽：帧缓冲里这块是连续的，一次 DMA 推完。 */
        rc = lcd_pixels_locked(&lcd_buf[(uint32_t)y * fb_w],
                               (size_t)rect_w * (size_t)(yend - y + 1));
    }
    else if (rc == EXIT_OK)
    {
        /* 窄矩形：帧缓冲里每行有跨距，只能一行一段地推。 */
        for (row = y; row <= yend && rc == EXIT_OK; row++)
        {
            rc = lcd_pixels_locked(&lcd_buf[(uint32_t)row * fb_w + x], rect_w);
        }
    }

    return lcd_txn_end(rc);
}

exit_code_t lcd_flush(void)
{
    if (lcd_self.width == 0 || lcd_self.height == 0)
    {
        return EXIT_NOT_INITIALIZED;
    }

    return lcd_flush_rect(0, 0, (uint16_t)(lcd_self.width - 1), (uint16_t)(lcd_self.height - 1));
}

uint16_t lcd_width(void)
{
    return lcd_self.width;
}

uint16_t lcd_height(void)
{
    return lcd_self.height;
}

uint16_t *lcd_fb(void)
{
    return lcd_buf;
}

static void lcd_fb_fill(uint16_t color)
{
    uint32_t n = (uint32_t)lcd_self.width * (uint32_t)lcd_self.height;
    uint32_t pair = ((uint32_t)color << 16) | color;
    uint32_t *p32 = (uint32_t *)(void *)lcd_buf;

    if (n > LCD_PIXEL_MAX)
    {
        n = LCD_PIXEL_MAX;
    }

    while (n >= 2)
    {
        *p32++ = pair;
        n -= 2;
    }
    if (n != 0)
    {
        *(uint16_t *)(void *)p32 = color;
    }
}

exit_code_t lcd_on(void)
{
    /* GPIO25 低电平打开 Q2(S8550)，点亮 LEDA。 */
    exit_code_t rc = bsp_gpio_set_active(BSP_GPIO_LCD_BL, true);

    lcd_delay_ms(10);
    return rc;
}

exit_code_t lcd_off(void)
{
    exit_code_t rc = bsp_gpio_set_active(BSP_GPIO_LCD_BL, false);

    lcd_delay_ms(10);
    return rc;
}

exit_code_t lcd_init(void)
{
    static const lcd_init_cmd_t init_cmds[] = {
        {0x11, {0}, 0x80},
        {0x3A, {0x05}, 1},
        {0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 5},
        {0xB7, {0x35}, 1},
        {0xBB, {0x19}, 1},
        {0xC0, {0x2C}, 1},
        {0xC2, {0x01}, 1},
        {0xC3, {0x12}, 1},
        {0xC4, {0x20}, 1},
        {0xC6, {0x01}, 1},
        {0xD0, {0xA4, 0xA1}, 2},
        {0xE0, {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23}, 14},
        {0xE1, {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23}, 14},
        {0x21, {0}, 0x80},
        {0x29, {0}, 0x80},
        {0, {0}, 0xff},
    };
    exit_code_t rc;

    /* 总线与 GPIO 表的初始化本来该由 app 统一做，这里兜一次底，
       重复调用返回 EXIT_ALREADY_INITIALIZED，当作成功。 */
    rc = bsp_gpio_init();
    if (rc != EXIT_OK && rc != EXIT_ALREADY_INITIALIZED)
    {
        return rc;
    }
    rc = bsp_spi_init();
    if (rc != EXIT_OK && rc != EXIT_ALREADY_INITIALIZED)
    {
        return rc;
    }

    lcd_self.dir = 0;

    /* 面板上电：背光先灭（bsp_gpio_init 已经把它置成灭），等电源稳定再发命令。 */
    rc = bsp_gpio_set_active(BSP_GPIO_LCD_BL, false);
    if (rc != EXIT_OK)
    {
        return rc;
    }
    lcd_delay_ms(120);

    for (uint32_t i = 0; init_cmds[i].databytes != 0xff; i++)
    {
        rc = lcd_write_cmd(init_cmds[i].cmd);
        if (rc == EXIT_OK)
        {
            rc = lcd_write_data(init_cmds[i].data, init_cmds[i].databytes & 0x1F);
        }
        if (rc != EXIT_OK)
        {
            return rc;
        }
        if (init_cmds[i].databytes & 0x80)
        {
            lcd_delay_ms(120);
        }
    }

    rc = lcd_display_dir(1);
    if (rc != EXIT_OK)
    {
        return rc;
    }

    lcd_fb_fill(0xFFFF);

    rc = lcd_flush();
    if (rc != EXIT_OK)
    {
        return rc;
    }

    return lcd_on();
}
