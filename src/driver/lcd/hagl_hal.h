/**
 * @file        hagl_hal.h
 * @brief       HAGL 后端契约：库通过这个头文件找到本工程的 HAL
 *
 * 文件名是库写死的：hagl.h:57 用 <hagl_hal.h> 包含它，hagl.c:69 在 hagl_init()
 * 里调用 hagl_hal_init() —— 而库里没有任何地方声明这个函数，不在这里声明就报
 * 隐式声明错误。两个名字都不要改。
 */

#ifndef HAGL_HAL_H
#define HAGL_HAL_H

#include "hagl/backend.h"
#include "hagl_hal_color.h"
#include "lib/tools/common_def.h"

/**
 * 填 backend。由 hagl_init() 调用（hagl.c:69），应用一般不直接调。
 *
 * 会把面板带起来（默认横屏 240x135）。想在别的方向下用 HAGL，就先自己
 * lcd_init() + lcd_display_dir() 再调 hagl_init() —— 这里发现面板已经起来
 * 就不会再动方向（HAGL 把 width/height 抄进 backend 并据此定 clip，见
 * hagl.c:70，所以方向必须在 hagl_init() 之前定好）。
 */
void hagl_hal_init(hagl_backend_t *backend);

/**
 * 面向应用的包装：必要时初始化面板，并把失败原因带出来。
 *
 * hagl_init() 返回的是指针、没法表达失败，所以要在调它之前用这个函数确认
 * 面板真的起来了。幂等，重复调用直接返回 EXIT_OK。
 */
exit_code_t lcd_hal_init(void);

#endif
