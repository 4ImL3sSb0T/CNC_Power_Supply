/**
 * @file        hagl_hal_color.h
 * @brief       HAGL 后端的像素类型
 *
 * 文件名和类型名都是库写死的：hagl/color.h:39 用 <hagl_hal_color.h> 找它，
 * hagl.h 里到处按 hagl_color_t 的宽度做指针运算。不要改名。
 */

#ifndef HAGL_HAL_COLOR_H
#define HAGL_HAL_COLOR_H

#include <stdint.h>

/* ST7789 是 RGB565，两个字节一个像素。 */
typedef uint16_t hagl_color_t;

#endif
