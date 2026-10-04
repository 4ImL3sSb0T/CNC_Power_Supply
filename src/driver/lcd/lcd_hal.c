/**
 * @file        lcd_hal.c
 * @brief       HAGL 后端：把 ST7789 帧缓冲接成一张 HAGL surface
 *
 * 分工：图元、裁剪、字体全在 HAGL 里（纯软件，见 hagl_clip.c 那套），本文件只做
 * 两件事——把像素写进 lcd_buf，以及 flush 时把脏矩形交给 lcd_flush_rect()。
 *
 * 之所以能这么薄，靠两个前提：
 *   1. 帧缓冲是自然顺序的 RGB565，和 hagl_color_t 完全一致。bsp_spi_transfer16()
 *      在 DSS=16 下自己按 MSB 先出，所以整条链路不需要任何字节交换。
 *   2. ST7789 的窗口寄存器只在 lcd_flush_rect() 里动，HAL 这层完全不碰总线。
 *
 * 线程模型：HAGL 自身没有任何锁。请从同一个任务里画和 flush；要多个任务同时画，
 * 得自己在应用层加锁。
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/lcd/lcd.h"
#include "hagl_hal.h"

/* ============================== 脏矩形 ============================== */

/* 空集用 x0 > x1 表示。HAGL 不会告诉后端它改了哪一块，只能自己攒。 */
static hagl_window_t lcd_hal_dirty = { UINT16_MAX, UINT16_MAX, 0, 0 };

static inline bool lcd_hal_dirty_empty(void)
{
    return lcd_hal_dirty.x0 > lcd_hal_dirty.x1;
}

static inline void lcd_hal_dirty_clear(void)
{
    lcd_hal_dirty = (hagl_window_t){ UINT16_MAX, UINT16_MAX, 0, 0 };
}

static inline void lcd_hal_mark(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if (x0 < lcd_hal_dirty.x0)
    {
        lcd_hal_dirty.x0 = x0;
    }
    if (y0 < lcd_hal_dirty.y0)
    {
        lcd_hal_dirty.y0 = y0;
    }
    if (x1 > lcd_hal_dirty.x1)
    {
        lcd_hal_dirty.x1 = x1;
    }
    if (y1 > lcd_hal_dirty.y1)
    {
        lcd_hal_dirty.y1 = y1;
    }
}

/* backend 就是 hagl_backend_t*。width / buffer 在 hagl_hal_init 里填好，
   热路径直接从它取，不再回调 lcd_width() / lcd_fb()。 */
static inline hagl_color_t *lcd_hal_row(hagl_backend_t *b, uint16_t y, uint16_t x)
{
    return (hagl_color_t *)b->buffer + (uint32_t)y * (uint16_t)b->width + x;
}

/* ============================== 图元 ============================== */

static void lcd_hal_put_pixel(void *self, int16_t x0, int16_t y0, hagl_color_t color)
{
    hagl_backend_t *b = self;

    /* 裁剪已由 hagl_pixel.c:41-54 做完，这里不用再查边界。 */
    *lcd_hal_row(b, (uint16_t)y0, (uint16_t)x0) = color;
    lcd_hal_mark((uint16_t)x0, (uint16_t)y0, (uint16_t)x0, (uint16_t)y0);
}

/* hline / vline 被 rectangle / fill_rectangle / circle / fill_circle / ellipse /
   fill_ellipse / polygon / rounded_rectangle 全用上了，连 hagl_clear() 也是走
   hagl_fill_rectangle —— 填掉它们省下的是每像素一次边界比较加一次函数指针调用。 */

static void lcd_hal_hline(void *self, int16_t x0, int16_t y0, uint16_t width, hagl_color_t color)
{
    hagl_backend_t *b = self;
    hagl_color_t *dst = lcd_hal_row(b, (uint16_t)y0, (uint16_t)x0);
    uint32_t n = width;

    /* width 经 hagl_hline.c:47-67 裁剪后保证 >= 1，x0 已在 clip 内。 */
    if (color == 0)
    {
        /* hagl_clear() 就走这条（整屏填 0x00），值得单独快一下。 */
        memset(dst, 0, (size_t)n * sizeof(hagl_color_t));
    }
    else
    {
        const uint32_t pair = ((uint32_t)color << 16) | color;

        /* 先补齐到 4 字节对齐，之后一次存两个像素。同 lcd.c 的 lcd_fb_fill()。 */
        if (((uintptr_t)dst & 2u) != 0)
        {
            *dst++ = color;
            n--;
        }
        while (n >= 2)
        {
            *(uint32_t *)(void *)dst = pair;
            dst += 2;
            n -= 2;
        }
        if (n != 0)
        {
            *dst = color;
        }
    }

    lcd_hal_mark((uint16_t)x0, (uint16_t)y0, (uint16_t)(x0 + width - 1), (uint16_t)y0);
}

static void lcd_hal_vline(void *self, int16_t x0, int16_t y0, uint16_t height, hagl_color_t color)
{
    hagl_backend_t *b = self;
    hagl_color_t *dst = lcd_hal_row(b, (uint16_t)y0, (uint16_t)x0);
    const uint32_t stride = (uint16_t)b->width;

    /* 竖线跨行、行距是屏幕宽度，没法像 hline 那样按 32 位合并。 */
    for (uint32_t i = 0; i < height; i++)
    {
        *dst = color;
        dst += stride;
    }

    lcd_hal_mark((uint16_t)x0, (uint16_t)y0, (uint16_t)x0, (uint16_t)(y0 + height - 1));
}

static hagl_color_t lcd_hal_get_pixel(void *self, int16_t x0, int16_t y0)
{
    hagl_backend_t *b = self;

    /* 越界已由 hagl_pixel.c:59-73 拦掉并返回黑色。 */
    return *lcd_hal_row(b, (uint16_t)y0, (uint16_t)x0);
}

/*
 * backend.h:57 把 blit 的坐标声明成 int16_t，surface.h:53 却声明成 uint16_t，
 * 上游自己不一致。库是照 surface.h 的类型调进来的（hagl_blit.c:62），这里按
 * backend.h 定义，好让赋值不产生函数指针类型告警 —— AAPCS 下这两种窄整数都走
 * 同一个 32 位寄存器，坐标又经裁剪保证非负，两者位模式一致，没有实际差别。
 */
static void lcd_hal_blit(void *self, int16_t x0, int16_t y0, hagl_bitmap_t *src)
{
    hagl_backend_t *b = self;
    hagl_color_t *dst = lcd_hal_row(b, (uint16_t)y0, (uint16_t)x0);
    const uint8_t *row = src->buffer;
    const size_t row_bytes = (size_t)src->width * sizeof(hagl_color_t);

    /* HAGL 只在源完全落在 clip 内时才调到这里（hagl_blit.c:47-63），不用再查边界。 */
    for (uint16_t r = 0; r < src->height; r++)
    {
        memcpy(dst, row, row_bytes);
        dst += (uint16_t)b->width;      /* 目标行距 = 屏幕宽度 */
        row += src->pitch;              /* 源行距 = bitmap 的 pitch，单位是字节 */
    }

    lcd_hal_mark((uint16_t)x0, (uint16_t)y0,
                 (uint16_t)(x0 + src->width - 1), (uint16_t)(y0 + src->height - 1));
}

static void lcd_hal_scale_blit(void *self, uint16_t x0, uint16_t y0, uint16_t w, uint16_t h,
                               hagl_bitmap_t *src)
{
    hagl_backend_t *b = self;
    hagl_color_t *fb = (hagl_color_t *)b->buffer;
    const uint16_t fb_w = (uint16_t)b->width;
    const uint16_t fb_h = (uint16_t)b->height;
    uint32_t x_ratio;
    uint32_t y_ratio;

    /* 上游直接转发缩放绘制，后端负责屏幕边界与用户 clip 的交集。 */
    if (w == 0 || h == 0 || b->width <= 0 || b->height <= 0 || fb == NULL ||
        src == NULL || src->buffer == NULL || src->width == 0 || src->height == 0)
    {
        return;
    }
    uint32_t left = x0 > b->clip.x0 ? x0 : b->clip.x0;
    uint32_t top = y0 > b->clip.y0 ? y0 : b->clip.y0;
    uint32_t right = (uint32_t)x0 + w;
    uint32_t bottom = (uint32_t)y0 + h;
    if (right > fb_w) right = fb_w;
    if (right > (uint32_t)b->clip.x1 + 1u) right = (uint32_t)b->clip.x1 + 1u;
    if (bottom > fb_h) bottom = fb_h;
    if (bottom > (uint32_t)b->clip.y1 + 1u) bottom = (uint32_t)b->clip.y1 + 1u;
    if (left >= right || top >= bottom) return;

    /* 缩放比例始终使用原始目标尺寸；裁剪只改变写入范围和源坐标偏移。 */
    x_ratio = ((uint32_t)src->width << 16) / w;
    y_ratio = ((uint32_t)src->height << 16) / h;
    for (uint32_t y = top; y < bottom; y++)
    {
        const uint32_t sy = ((y - y0) * y_ratio) >> 16;
        const hagl_color_t *srow = (const hagl_color_t *)(src->buffer + sy * src->pitch);
        hagl_color_t *drow = fb + y * fb_w;
        for (uint32_t x = left; x < right; x++)
        {
            drow[x] = srow[((x - x0) * x_ratio) >> 16];
        }
    }
    lcd_hal_mark((uint16_t)left, (uint16_t)top,
                 (uint16_t)(right - 1u), (uint16_t)(bottom - 1u));
}

/* ========================== 呈现与收尾 ========================== */

static size_t lcd_hal_flush(void *self)
{
    (void)self;

    if (!lcd_hal_dirty_empty())
    {
        const exit_code_t rc = lcd_flush_rect(lcd_hal_dirty.x0, lcd_hal_dirty.y0,
                                              lcd_hal_dirty.x1, lcd_hal_dirty.y1);

        /* 发失败就把脏矩形留着，下一轮连新画的区域一起重发；宁可重发也不丢帧。 */
        if (rc == EXIT_OK)
        {
            lcd_hal_dirty_clear();
        }
    }
    return 0;
}

static void lcd_hal_close(void *self)
{
    (void)self;
    (void)lcd_off();
}

/* ============================== 初始化 ============================== */

static bool lcd_hal_panel_ready = false;

exit_code_t lcd_hal_init(void)
{
    exit_code_t rc;

    if (lcd_hal_panel_ready)
    {
        return EXIT_OK;
    }

    /* 应用可能已经自己 lcd_init() + lcd_display_dir() 选好了方向，那就别再动，
       否则会被重置回默认横屏。 */
    if (lcd_width() == 0 || lcd_height() == 0)
    {
        rc = lcd_init();
        if (rc != EXIT_OK)
        {
            return rc;
        }
    }

    lcd_hal_panel_ready = true;
    return EXIT_OK;
}

void hagl_hal_init(hagl_backend_t *backend)
{
    /* 应用可以先自己调 lcd_hal_init() 把失败码拿走；没调的话这里兜底。
       hagl.c:67 已经把 backend 清过零了。 */
    (void)lcd_hal_init();

    backend->width = (int16_t)lcd_width();
    backend->height = (int16_t)lcd_height();
    backend->depth = 16;
    backend->buffer = (uint8_t *)lcd_fb();

    backend->put_pixel = lcd_hal_put_pixel;
    backend->hline = lcd_hal_hline;
    backend->vline = lcd_hal_vline;
    backend->get_pixel = lcd_hal_get_pixel;
    backend->blit = lcd_hal_blit;
    backend->scale_blit = lcd_hal_scale_blit;
    backend->flush = lcd_hal_flush;
    backend->close = lcd_hal_close;

    /* color 留空：hagl_color.c:42-45 回落到 rgb565()，正是本面板的格式。
       buffer2 留空：单缓冲，库自己也从不读它。 */

    lcd_hal_dirty_clear();
}
