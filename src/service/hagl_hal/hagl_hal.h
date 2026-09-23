/**
 * @file        hagl_hal.h
 * @brief       HAGL backend：写 lcd_fb()，flush 走 lcd_flush()
 *
 * 不用官方 hagl_pico_mipi（它会自己占 SPI/DMA）。
 * 必须先 lcd_init() 再 hagl_init()。圆 / 字 / clip 留给 HAGL。
 */

#ifndef HAGL_HAL_H
#define HAGL_HAL_H

#include <hagl/backend.h>
#include "hagl_hal_color.h"

#ifndef HAGL_CHAR_BUFFER_SIZE
#define HAGL_CHAR_BUFFER_SIZE   (6 * 9 * 2)
#endif

void hagl_hal_init(hagl_backend_t *backend);

#endif
