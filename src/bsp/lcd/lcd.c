/**
 * @file        lcd.c
 * @brief       ST7789 1.14" 面板：初始化、方向、窗口、单缓冲 flush
 *
 * Derived from ALIENTEK RP2350A LCD example.
 * 移植注记：原版的 sleep_ms() 在 RP2350 上可能永不返回（SDK issue #3078，
 * sleep 走 __wfe() + alarm，偶发 alarm 未唤醒），这里全部改用 busy_wait_ms()。
 */

#include "bsp/lcd/lcd_priv.h"

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

static uint16_t lcd_colstart;
static uint16_t lcd_rowstart;

typedef struct
{
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes;      /* bit7 = 发送后延时；0xFF = 结束 */
} lcd_init_cmd_t;

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t buf[4];
    uint16_t xs = (uint16_t)(x0 + lcd_colstart);
    uint16_t xe = (uint16_t)(x1 + lcd_colstart);
    uint16_t ys = (uint16_t)(y0 + lcd_rowstart);
    uint16_t ye = (uint16_t)(y1 + lcd_rowstart);

    buf[0] = (uint8_t)(xs >> 8);
    buf[1] = (uint8_t)xs;
    buf[2] = (uint8_t)(xe >> 8);
    buf[3] = (uint8_t)xe;
    lcd_write_cmd(0x2A);
    lcd_write_data(buf, 4);

    buf[0] = (uint8_t)(ys >> 8);
    buf[1] = (uint8_t)ys;
    buf[2] = (uint8_t)(ye >> 8);
    buf[3] = (uint8_t)ye;
    lcd_write_cmd(0x2B);
    lcd_write_data(buf, 4);

    lcd_write_cmd(0x2C);
}

void lcd_display_dir(uint8_t dir)
{
    const lcd_orient_t *o = &lcd_orient[dir ? 1 : 0];
    uint8_t madctl = o->madctl;

    lcd_self.dir = dir ? 1 : 0;
    lcd_self.width = o->width;
    lcd_self.height = o->height;
    lcd_colstart = o->colstart;
    lcd_rowstart = o->rowstart;

    lcd_write_cmd(0x36);
    lcd_write_data(&madctl, 1);
    lcd_set_window(0, 0, (uint16_t)(lcd_self.width - 1), (uint16_t)(lcd_self.height - 1));
}

void lcd_flush_rect(uint16_t x, uint16_t y, uint16_t xend, uint16_t yend)
{
    uint16_t row;
    uint16_t fb_w;
    uint16_t rect_w;

    if (lcd_self.width == 0 || lcd_self.height == 0)
    {
        return;
    }
    if (x >= lcd_self.width || y >= lcd_self.height)
    {
        return;
    }
    if (xend >= lcd_self.width)
    {
        xend = (uint16_t)(lcd_self.width - 1);
    }
    if (yend >= lcd_self.height)
    {
        yend = (uint16_t)(lcd_self.height - 1);
    }
    if (x > xend || y > yend)
    {
        return;
    }

    fb_w = lcd_self.width;
    rect_w = (uint16_t)(xend - x + 1);

    lcd_wait_idle();
    lcd_set_window(x, y, xend, yend);
    LCD_DC(1);
    LCD_CS(0);
    lcd_spi_16bit();

    if (x == 0 && xend == (uint16_t)(fb_w - 1))
    {
        lcd_dma_start(&lcd_buf[(uint32_t)y * fb_w],
                      (size_t)rect_w * (size_t)(yend - y + 1));
        return;
    }

    for (row = y; row <= yend; row++)
    {
        lcd_dma_start(&lcd_buf[(uint32_t)row * fb_w + x], rect_w);
        if (row < yend)
        {
            lcd_wait_idle();
            LCD_DC(1);
            LCD_CS(0);
            lcd_spi_16bit();
        }
    }
}

void lcd_flush(void)
{
    if (lcd_self.width == 0 || lcd_self.height == 0)
    {
        return;
    }
    lcd_flush_rect(0, 0, (uint16_t)(lcd_self.width - 1), (uint16_t)(lcd_self.height - 1));
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

void lcd_fb_lock(void)
{
    lcd_wait_idle();
}

static void lcd_fb_fill(uint16_t color)
{
    uint32_t n = (uint32_t)lcd_self.width * (uint32_t)lcd_self.height;
    uint32_t pair = ((uint32_t)color << 16) | color;
    uint32_t *p32 = (uint32_t *)(void *)lcd_buf;

    if (n > LCD_PIXEL_MAX) {
        n = LCD_PIXEL_MAX;
    }

    while (n >= 2) {
        *p32++ = pair;
        n -= 2;
    }
    if (n != 0) {
        *(uint16_t *)(void *)p32 = color;
    }
}

void lcd_on(void)
{
    /* GPIO25 低电平打开 Q2(S8550)，点亮 LEDA。 */
    LCD_BL(0);
    busy_wait_ms(10);
}

void lcd_off(void)
{
    LCD_BL(1);
    busy_wait_ms(10);
}

void lcd_init(void)
{
    int cmd = 0;
    lcd_init_cmd_t ili_init_cmds[] = {
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

    lcd_self.dir = 0;
    lcd_bus_init();

    gpio_init(LCD_PIN_DC);
    gpio_set_dir(LCD_PIN_DC, GPIO_OUT);
    gpio_put(LCD_PIN_DC, 1);

    gpio_init(LCD_PIN_CS);
    gpio_set_dir(LCD_PIN_CS, GPIO_OUT);
    gpio_put(LCD_PIN_CS, 1);

    gpio_init(LCD_PIN_BL);
    gpio_set_dir(LCD_PIN_BL, GPIO_OUT);
    gpio_put(LCD_PIN_BL, 1);

    busy_wait_ms(120);

    while (ili_init_cmds[cmd].databytes != 0xff)
    {
        lcd_write_cmd(ili_init_cmds[cmd].cmd);
        lcd_write_data(ili_init_cmds[cmd].data, ili_init_cmds[cmd].databytes & 0x1F);
        if (ili_init_cmds[cmd].databytes & 0x80)
        {
            busy_wait_ms(120);
        }
        cmd++;
    }

    lcd_display_dir(1);
    lcd_fb_fill(0xFFFF);
    lcd_flush();
    lcd_on();
}
