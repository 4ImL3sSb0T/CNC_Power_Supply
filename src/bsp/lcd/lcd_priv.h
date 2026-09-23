/**
 * @file        lcd_priv.h
 * @brief       LCD 驱动内部接口（总线 / 面板 / 帧缓冲）
 */

#ifndef LCD_PRIV_H
#define LCD_PRIV_H

#include "bsp/lcd/lcd.h"
#include "bsp/lcd/spi.h"

#define LCD_PIN_BL              25
#define LCD_PIN_DC              8
#define LCD_PIN_CS              9

#define LCD_DC(x)               gpio_put(LCD_PIN_DC, (x))
#define LCD_CS(x)               gpio_put(LCD_PIN_CS, (x))
#define LCD_BL(x)               gpio_put(LCD_PIN_BL, (x))

#define LCD_PIXEL_MAX           (240 * 135)

typedef struct
{
    uint16_t width;
    uint16_t height;
    uint8_t  dir;       /* 0 竖屏 135x240，1 横屏 240x135 */
} lcd_obj_t;

extern lcd_obj_t lcd_self;
extern uint16_t lcd_buf[LCD_PIXEL_MAX];

void lcd_bus_init(void);
void lcd_spi_8bit(void);
void lcd_spi_16bit(void);
void lcd_dma_start(const uint16_t *src, size_t count);
void lcd_write_cmd(uint8_t cmd);
void lcd_write_data(const uint8_t *data, int len);

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

#endif
