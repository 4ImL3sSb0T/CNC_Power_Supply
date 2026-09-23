/**
 * @file        lcd.h
 * @brief       ST7789 1.14" (240x135) 单缓冲驱动
 *
 * 设备 + 帧缓冲所有权。不提供画圆 / 画线 / 字体。
 * 改 lcd_fb() 前必须 lcd_fb_lock()；呈现用 lcd_flush / lcd_flush_rect。
 * 单缓冲：flush 启动 DMA 后立刻返回。
 */

#ifndef __LCD_H__
#define __LCD_H__

#include "pico/stdlib.h"

void lcd_init(void);
void lcd_on(void);
void lcd_off(void);
void lcd_display_dir(uint8_t dir);

uint16_t lcd_width(void);
uint16_t lcd_height(void);
uint16_t *lcd_fb(void);
void lcd_fb_lock(void);

void lcd_wait_idle(void);
bool lcd_is_busy(void);
void lcd_flush(void);
void lcd_flush_rect(uint16_t x, uint16_t y, uint16_t xend, uint16_t yend);

#endif
