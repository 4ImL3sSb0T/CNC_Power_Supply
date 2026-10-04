#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../../src/driver/lcd/lcd_hal.c"

static uint16_t panel[240 * 135];
exit_code_t lcd_init(void) { return EXIT_OK; }
exit_code_t lcd_off(void) { return EXIT_OK; }
uint16_t lcd_width(void) { return 240; }
uint16_t lcd_height(void) { return 135; }
uint16_t *lcd_fb(void) { return panel; }
exit_code_t lcd_flush_rect(uint16_t x, uint16_t y, uint16_t xe, uint16_t ye) {
    (void)x; (void)y; (void)xe; (void)ye; return EXIT_OK;
}

static void compare_crop(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h,
                          hagl_window_t clip) {
    // 大画布画完整图，再与小画布裁剪后的结果比较，检查缩放比例和源坐标偏移。
    uint16_t reference[320 * 240] = {0};
    uint16_t guarded[240 * 135 + 2] = {0};
    uint16_t source[4 * 10];
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 10; x++) source[y * 10 + x] = (uint16_t)(y * 10 + x + 1);
    }
    guarded[0] = guarded[240 * 135 + 1] = 0xCAFE;
    hagl_bitmap_t bitmap = {.width = 8, .height = 4, .depth = 16,
                            .pitch = 20, .buffer = (uint8_t *)source};
    hagl_backend_t full = {.width = 320, .height = 240, .clip = {0, 0, 319, 239},
                           .buffer = (uint8_t *)reference};
    hagl_backend_t cropped = {.width = 240, .height = 135, .clip = clip,
                              .buffer = (uint8_t *)(guarded + 1)};
    lcd_hal_scale_blit(&full, x0, y0, w, h, &bitmap);
    lcd_hal_dirty_clear();
    lcd_hal_scale_blit(&cropped, x0, y0, w, h, &bitmap);
    for (unsigned y = 0; y < 135; y++) {
        for (unsigned x = 0; x < 240; x++) {
            const uint16_t expected = x >= clip.x0 && x <= clip.x1 &&
                                      y >= clip.y0 && y <= clip.y1 ? reference[y * 320 + x] : 0;
            assert(guarded[1 + y * 240 + x] == expected);
        }
    }
    assert(guarded[0] == 0xCAFE && guarded[240 * 135 + 1] == 0xCAFE);
}

int main(void) {
    compare_crop(200, 100, 80, 40, (hagl_window_t){0, 0, 239, 134});
    compare_crop(10, 10, 80, 40, (hagl_window_t){30, 20, 59, 39});
    compare_crop(230, 130, 80, 40, (hagl_window_t){0, 0, 239, 134});
    compare_crop(0, 0, 80, 40, (hagl_window_t){200, 100, 239, 134});
    compare_crop(250, 150, 20, 20, (hagl_window_t){0, 0, 239, 134});
    compare_crop(0, 0, 80, 40, (hagl_window_t){20, 20, 10, 10});
    hagl_backend_t b = {.width = 240, .height = 135, .buffer = (uint8_t *)panel,
                        .clip = {0, 0, 239, 134}};
    lcd_hal_dirty_clear();
    lcd_hal_scale_blit(&b, 0, 0, 0, 10, NULL);
    lcd_hal_scale_blit(&b, 0, 0, 10, 10, NULL);
    assert(lcd_hal_dirty_empty());
    puts("PASS: scaled bitmap clipping, source pitch and buffer guards");
    return 0;
}
