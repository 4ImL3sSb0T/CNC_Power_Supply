#pragma once
#include "hardware/gpio.h"
#include "lib/tools/common_def.h"
#include "stdint.h"

typedef enum {
    BSP_GPIO_LED_STATUS,
    BSP_GPIO_KEY_MAIN,
    BSP_GPIO_KEY_AUX,
    BSP_GPIO_KEY_UP,
    BSP_GPIO_KEY_DOWN,
} BSP_GPIO_CH;

typedef enum { BSP_GPIO_DIR_IN, BSP_GPIO_DIR_OUT } bsp_gpio_dir_t;
typedef enum { BSP_GPIO_PULL_NONE, BSP_GPIO_PULL_UP, BSP_GPIO_PULL_DOWN } bsp_gpio_pull_t;
typedef enum { BSP_GPIO_IRQ_NONE, BSP_GPIO_IRQ_RISING, BSP_GPIO_IRQ_FALLING, BSP_GPIO_IRQ_BOTH } bsp_gpio_irq_t;
typedef void (*bsp_gpio_isr_handle)(BSP_GPIO_CH ch, void *ctx);

typedef struct {
    bsp_gpio_isr_handle isr;
    void *ctx;
    bsp_gpio_dir_t dir;
    bsp_gpio_pull_t pull;
    bsp_gpio_irq_t irq;
    u32 gpio;
    bool active_low;
    bool init_active;
} bsp_gpio_ch_t;

exit_code_t bsp_gpio_init();

bool bsp_gpio_get_active(BSP_GPIO_CH ch);
exit_code_t bsp_gpio_set_active(BSP_GPIO_CH ch, bool active);
exit_code_t bsp_gpio_toggle_active(BSP_GPIO_CH ch);

exit_code_t bsp_gpio_irq_attach(BSP_GPIO_CH ch, bsp_gpio_irq_t edge, bsp_gpio_isr_handle isr, void *ctx);
exit_code_t bsp_gpio_irq_detach(BSP_GPIO_CH ch);

