/**
 * @file        key.h
 * @brief       测试台按键 BSP（KEY0 / GPIO2 + MultiButton）
 *
 * MultiButton 状态机由 FreeRTOS 任务每 5ms 调一次 key_tick() 打拍
 * （不依赖 async_context）。单击/双击/长按判定时间在这里改，
 * 不要直接改 multi_button.h。
 */

#ifndef KEY_H
#define KEY_H

#include "lib/tools/common_def.h"
#include "bsp/input/multi_button.h"

#define TICKS_INTERVAL          5       /* ms，MultiButton 打拍周期 */
#define KEY_SHORT_MS            100     /* 松手后再等这么久才判定单击，以便区分双击 */
#define KEY_LONG_MS             1000
#define SHORT_TICKS             (KEY_SHORT_MS / TICKS_INTERVAL)
#define LONG_TICKS              (KEY_LONG_MS / TICKS_INTERVAL)

/* 引脚表 KEY0：GPIO2，按下为低，内部上拉（勿开内部下拉，RP2350-E9） */
#define KEY0_GPIO_PIN           2
#define KEY0_ACTIVE_LEVEL       0

typedef enum {
    KEY_ID_0 = 0,
    KEY_COUNT
} key_id_t;

void key_init(void);
void key_tick(void);
void key_attach(key_id_t id, ButtonEvent event, BtnCallback cb);
int key_is_pressed(key_id_t id);

#endif
