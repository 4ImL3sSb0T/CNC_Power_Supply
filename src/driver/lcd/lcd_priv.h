/**
 * @file        lcd_priv.h
 * @brief       LCD 驱动内部接口（面板协议 / 帧缓冲）
 */

#ifndef LCD_PRIV_H
#define LCD_PRIV_H

#include <stddef.h>
#include <stdint.h>

#include "bsp/gpio/bsp_gpio.h"
#include "bsp/spi/bsp_spi.h"
#include "driver/lcd/lcd.h"

/* 帧缓冲上限：240x135 与 135x240 两种方向里取大的那份。 */
#define LCD_PIXEL_MAX           (240 * 135)

/* 一次 SPI 事务的超时。整屏 64800 字节 @40MHz 约 13ms，这里留足余量。 */
#define LCD_TXN_TIMEOUT_MS      100u

typedef struct
{
    uint16_t width;
    uint16_t height;
    uint16_t colstart;      /* 可见区在 GRAM 里的偏移 */
    uint16_t rowstart;
    uint8_t  dir;           /* 0 竖屏 135x240，1 横屏 240x135 */
} lcd_obj_t;

extern lcd_obj_t lcd_self;
extern uint16_t lcd_buf[LCD_PIXEL_MAX];

/*
 * 三段式事务：begin 拿总线并拉低 CS，中间可任意调 _locked 系列，最后 end 收尾。
 *
 * 之所以要拆开：设窗口（0x2A/0x2B/0x2C）和推像素必须落在同一个 CS 窗口里，
 * 但分开看又是几个独立的传输。
 *
 * 契约同 bsp_spi：
 *   - begin 失败时既没拿锁也没拉 CS，此时绝不能调 end；
 *   - begin 成功之后，任何失败路径都必须走到 end，否则 CS 永远拉低。
 *   - end 会把 prior 的失败优先返回，调用方可以直接 return lcd_txn_end(rc)。
 */
exit_code_t lcd_txn_begin(void);
exit_code_t lcd_txn_end(exit_code_t prior);

/* 下面几个都要求已持有事务（在 begin 与 end 之间）。 */
exit_code_t lcd_cmd_locked(uint8_t cmd);
exit_code_t lcd_data_locked(const uint8_t *data, size_t len);
exit_code_t lcd_window_locked(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
exit_code_t lcd_pixels_locked(const uint16_t *pixels, size_t count);

/* 自成一体的短事务（内部 begin + 一次调用 + end），给初始化和方向设置用。 */
exit_code_t lcd_write_cmd(uint8_t cmd);
exit_code_t lcd_write_data(const uint8_t *data, size_t len);
exit_code_t lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

#endif
