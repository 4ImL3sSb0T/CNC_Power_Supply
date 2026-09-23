/**
 * @file        hagl_hal.c
 * @brief       把 HAGL 接到本板单缓冲：像素进 lcd_fb()，flush 进 DMA
 */

#include "hagl_hal.h"

#include <stdbool.h>

#include "bsp/lcd/lcd.h"
#include "rgb565.h"

static bool fb_locked;

static void ensure_fb(void)
{
    if (!fb_locked) {
        lcd_fb_lock();
        fb_locked = true;
    }
}

static void fill_span(uint16_t *dst, uint32_t count, uint16_t color)
{
    uint32_t pair;
    uint32_t *p32;

    if ((dst == NULL) || (count == 0)) {
        return;
    }

    if (((uintptr_t)dst & 2u) != 0u) {
        *dst++ = color;
        count--;
    }

    pair = ((uint32_t)color << 16) | color;
    p32 = (uint32_t *)(void *)dst;
    while (count >= 2) {
        *p32++ = pair;
        count -= 2;
    }
    if (count != 0) {
        *(uint16_t *)(void *)p32 = color;
    }
}

static void put_pixel(void *self, int16_t x0, int16_t y0, hagl_color_t color)
{
    uint16_t w = lcd_width();
    uint16_t h = lcd_height();

    (void)self;
    if ((x0 < 0) || (y0 < 0) || (x0 >= (int16_t)w) || (y0 >= (int16_t)h)) {
        return;
    }
    ensure_fb();
    lcd_fb()[(uint32_t)y0 * w + (uint16_t)x0] = color;
}

static hagl_color_t get_pixel(void *self, int16_t x0, int16_t y0)
{
    uint16_t w = lcd_width();
    uint16_t h = lcd_height();

    (void)self;
    if ((x0 < 0) || (y0 < 0) || (x0 >= (int16_t)w) || (y0 >= (int16_t)h)) {
        return 0;
    }
    ensure_fb();
    return lcd_fb()[(uint32_t)y0 * w + (uint16_t)x0];
}

static void hline(void *self, int16_t x0, int16_t y0, uint16_t width, hagl_color_t color)
{
    uint16_t w = lcd_width();
    uint16_t h = lcd_height();

    (void)self;
    if ((width == 0) || (y0 < 0) || (y0 >= (int16_t)h)) {
        return;
    }
    if (x0 < 0) {
        if ((int32_t)width <= (int32_t)(-x0)) {
            return;
        }
        width = (uint16_t)((int32_t)width + x0);
        x0 = 0;
    }
    if ((uint16_t)x0 >= w) {
        return;
    }
    if (((uint32_t)x0 + width) > w) {
        width = (uint16_t)(w - (uint16_t)x0);
    }

    ensure_fb();
    fill_span(&lcd_fb()[(uint32_t)y0 * w + (uint16_t)x0], width, color);
}

static void vline(void *self, int16_t x0, int16_t y0, uint16_t height, hagl_color_t color)
{
    uint16_t w = lcd_width();
    uint16_t h = lcd_height();
    uint16_t *fb;
    uint16_t i;

    (void)self;
    if ((height == 0) || (x0 < 0) || (x0 >= (int16_t)w)) {
        return;
    }
    if (y0 < 0) {
        if ((int32_t)height <= (int32_t)(-y0)) {
            return;
        }
        height = (uint16_t)((int32_t)height + y0);
        y0 = 0;
    }
    if ((uint16_t)y0 >= h) {
        return;
    }
    if (((uint32_t)y0 + height) > h) {
        height = (uint16_t)(h - (uint16_t)y0);
    }

    ensure_fb();
    fb = lcd_fb();
    for (i = 0; i < height; i++) {
        fb[(uint32_t)((uint16_t)y0 + i) * w + (uint16_t)x0] = color;
    }
}

static size_t flush(void *self)
{
    uint16_t w = lcd_width();
    uint16_t h = lcd_height();

    (void)self;
    lcd_flush();
    fb_locked = false;
    return (size_t)w * (size_t)h * sizeof(uint16_t);
}

static hagl_color_t make_color(void *self, uint8_t r, uint8_t g, uint8_t b)
{
    (void)self;
    return rgb565(r, g, b);
}

void hagl_hal_init(hagl_backend_t *backend)
{
    uint16_t w = lcd_width();
    uint16_t h = lcd_height();

    if ((w == 0) || (h == 0)) {
        return;
    }

    backend->width = (int16_t)w;
    backend->height = (int16_t)h;
    backend->depth = 16;
    backend->put_pixel = put_pixel;
    backend->get_pixel = get_pixel;
    backend->color = make_color;
    backend->hline = hline;
    backend->vline = vline;
    backend->flush = flush;
    backend->buffer = (uint8_t *)lcd_fb();
}
