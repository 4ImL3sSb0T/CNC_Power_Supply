/**
 * @file        key.c
 * @brief       KEY0（GPIO2）MultiButton 适配：key_tick() 每 5ms 打一次拍
 *
 * MultiButton 消抖发生在 IDLE，必须持续打拍，不能"空闲就停"。
 */

#include "bsp/input/key.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

static const u8 key_pins[KEY_COUNT] = {
    [KEY_ID_0] = KEY0_GPIO_PIN,
};

static Button s_btn[KEY_COUNT];

static u8 key_read_level(u8 button_id)
{
    if (button_id >= KEY_COUNT) {
        return (u8)!KEY0_ACTIVE_LEVEL;
    }
    return (u8)gpio_get(key_pins[button_id]);
}

void key_init(void)
{
    u8 i;

    for (i = 0; i < KEY_COUNT; i++) {
        gpio_init(key_pins[i]);
        gpio_set_dir(key_pins[i], GPIO_IN);
        gpio_pull_up(key_pins[i]);
        button_init(&s_btn[i], key_read_level, KEY0_ACTIVE_LEVEL, i);
        button_start(&s_btn[i]);
    }
}

void key_tick(void)
{
    button_ticks();
}

void key_attach(key_id_t id, ButtonEvent event, BtnCallback cb)
{
    if (id >= KEY_COUNT) {
        return;
    }
    button_attach(&s_btn[id], event, cb);
}

int key_is_pressed(key_id_t id)
{
    if (id >= KEY_COUNT) {
        return -1;
    }
    return button_is_pressed(&s_btn[id]);
}
