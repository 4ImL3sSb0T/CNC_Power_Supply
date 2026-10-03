/**
 * @file        lcd.h
 * @brief       ST7789 1.14" (240x135) 单缓冲驱动
 *
 * 设备 + 帧缓冲所有权。不提供画圆 / 画线 / 字体。
 *
 * 总线（CS / DMA / 时钟 / 超时恢复）全部由 bsp_spi 承担，DC 与背光由 bsp_gpio
 * 承担，本驱动只负责面板协议和帧缓冲布局。
 *
 * 呈现是阻塞的：lcd_flush() / lcd_flush_rect() 返回时该区域已经发完，
 * 所以改完 lcd_fb() 直接 flush 即可，不需要额外的锁或忙等。
 *
 * lcd_init() 必须在调度器启动后、从任务里调用。
 */

#ifndef __LCD_H__
#define __LCD_H__

#include <stdint.h>

#include "lib/tools/common_def.h"

exit_code_t lcd_init(void);
exit_code_t lcd_on(void);
exit_code_t lcd_off(void);
exit_code_t lcd_display_dir(uint8_t dir);

uint16_t lcd_width(void);
uint16_t lcd_height(void);
uint16_t *lcd_fb(void);

exit_code_t lcd_flush(void);
exit_code_t lcd_flush_rect(uint16_t x, uint16_t y, uint16_t xend, uint16_t yend);

#endif
