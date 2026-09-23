/**
 * @file        ui.c
 * @brief       测试台屏幕界面（240x135 横屏）
 *
 * 全工程唯一的 HAGL 使用者，单缓冲帧 + 50ms 全屏刷。字库为 ISO8859-1
 * 单字节，显示不了中文，界面文案全部用英文短词。
 */

#include "app/ui/ui.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "FreeRTOS.h"
#include "task.h"

#include "app/pcb_test/pcb_test.h"
#include "config/board_config.h"
#include "font6x9-ISO8859-1.h"

#define WHITE                   0xFFFFu
#define RED                     0xF800u
#define GREEN                   0x07E0u
#define YELLOW                  0xFFE0u
#define GRAY                    0x8430u
#define DARK                    0x18E3u
#define CYAN                    0x07FFu

#define ITEM_ROW_Y0             42
#define ITEM_ROW_PITCH          12
#define RESULT_X                212

static void ascii_to_wchar(const char *src, wchar_t *dst, size_t n)
{
    size_t i;

    if (n == 0) {
        return;
    }
    for (i = 0; (i + 1) < n && src[i] != '\0'; i++) {
        dst[i] = (wchar_t)(unsigned char)src[i];
    }
    dst[i] = 0;
}

static void put_str(hagl_backend_t *d, const char *s, i16 x, i16 y, u16 color)
{
    wchar_t wtext[48];

    ascii_to_wchar(s, wtext, sizeof(wtext) / sizeof(wtext[0]));
    hagl_put_text(d, wtext, x, y, color, font6x9_ISO8859_1);
}

static void fmt_mv(char *dst, size_t n, u16 mv)
{
    snprintf(dst, n, "%u.%02u", (unsigned)(mv / 1000u), (unsigned)((mv % 1000u) / 10u));
}

static u16 result_color(test_result_t r)
{
    switch (r) {
    case TRES_RUNNING: return CYAN;
    case TRES_PASS:    return GREEN;
    case TRES_FAIL:    return RED;
    case TRES_WARN:    return YELLOW;
    case TRES_MANUAL:  return CYAN;
    default:           return GRAY;
    }
}

static const char *result_text(test_result_t r)
{
    switch (r) {
    case TRES_RUNNING: return "RUN ";
    case TRES_PASS:    return "PASS";
    case TRES_FAIL:    return "FAIL";
    case TRES_WARN:    return "WARN";
    case TRES_SKIP:    return "SKIP";
    case TRES_MANUAL:  return "MANL";
    default:           return "-   ";
    }
}

static const char *state_text(const test_status_t *s)
{
    if (s->manual_mode) {
        return "MANU";
    }
    switch (s->run_state) {
    case RUN_RUNNING:  return "RUN";
    case RUN_DONE:     return "DONE";
    case RUN_ABORTED:  return "STOP";
    default:           return "IDLE";
    }
}

static void ui_draw(hagl_backend_t *d, const test_status_t *s)
{
    char text[40];
    char vset[8];
    char vpg[8];
    char vil[8];
    const char *pg_str;
    const char *st;
    u16 pg_color;
    u16 st_color;
    u16 vset_mv;
    u16 ilim_ma;
    i16 st_x;
    u8 i;

    /* 设定值换算：VSET = (1+5D)/6 × 23.4V；ILIM2 = D × 5.04A */
    vset_mv = (u16)((PCB_VOUT_SET_MV * (1000u + 5u * s->pwm_permille)) / 6000u);
    ilim_ma = (u16)((PCB_ILIM2_FULL_MA * s->ipwm_permille) / 1000u);

    fmt_mv(vset, sizeof(vset), vset_mv);
    fmt_mv(vpg, sizeof(vpg), s->pg_mv);
    fmt_mv(vil, sizeof(vil), ilim_ma);

    if (s->pg_mv >= PCB_PG_GOOD_MIN_MV) {
        pg_str = "GOOD";
        pg_color = GREEN;
    } else if (s->pg_mv <= PCB_PG_FAULT_MAX_MV) {
        pg_str = "FAULT";
        pg_color = YELLOW;
    } else {
        pg_str = "???";
        pg_color = GRAY;
    }

    st = state_text(s);
    if (s->manual_mode) {
        st_color = YELLOW;
    } else if (s->run_state == RUN_RUNNING) {
        st_color = CYAN;
    } else if (s->run_state == RUN_DONE) {
        st_color = GREEN;
    } else if (s->run_state == RUN_ABORTED) {
        st_color = RED;
    } else {
        st_color = GRAY;
    }

    hagl_clear(d);
    hagl_fill_rectangle(d, 0, 0, 239, 15, DARK);
    put_str(d, "CNC PSU PCB TEST", 4, 3, WHITE);
    st_x = (i16)(236 - (i16)strlen(st) * 6);
    put_str(d, st, st_x, 3, st_color);
    hagl_draw_hline(d, 0, 16, 240, GRAY);

    snprintf(text, sizeof(text), "VSET %sV (%u%%)", vset, (unsigned)(s->pwm_permille / 10u));
    put_str(d, text, 4, 19, WHITE);
    snprintf(text, sizeof(text), "PG %sV %s", vpg, pg_str);
    put_str(d, text, 124, 19, pg_color);

    snprintf(text, sizeof(text), "ILIM %sA (%u%%)", vil, (unsigned)(s->ipwm_permille / 10u));
    put_str(d, text, 4, 31, CYAN);
    snprintf(text, sizeof(text), "CE %s", s->ce_on ? "ON" : "OFF");
    put_str(d, text, 124, 31, s->ce_on ? GREEN : GRAY);
    hagl_draw_hline(d, 0, 40, 240, GRAY);

    for (i = 0; i < TEST_ITEM_COUNT; i++) {
        i16 y = (i16)(ITEM_ROW_Y0 + (i16)i * ITEM_ROW_PITCH);
        const test_item_t *it = &s->items[i];

        put_str(d, it->name, 4, y, (it->result == TRES_RUNNING) ? WHITE : GRAY);
        put_str(d, it->detail, 68, y, CYAN);
        put_str(d, result_text(it->result), RESULT_X, y, result_color(it->result));
    }

    put_str(d, "click=RUN  dbl=VSET step  long=STOP", 4, 126, GRAY);
}

void ui_task(void *pvParameters)
{
    hagl_backend_t *display = (hagl_backend_t *)pvParameters;
    test_status_t snap;

    for (;;) {
        pcb_test_get_status(&snap);
        ui_draw(display, &snap);
        hagl_flush(display);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
