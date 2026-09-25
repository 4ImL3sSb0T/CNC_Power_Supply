#include "bsp_gpio.h"

bsp_gpio_ch_t bsp_gpio_ch[] = {
    [BSP_GPIO_LED_STATUS] = {},
    [BSP_GPIO_KEY_MAIN] = {},
    [BSP_GPIO_KEY_AUX] = {},
    [BSP_GPIO_KEY_UP] = {},
    [BSP_GPIO_KEY_DOWN] = {}
};

exit_code_t bsp_gpio_init() {
    
}

bool bsp_gpio_get_active(BSP_GPIO_CH ch);
exit_code_t bsp_gpio_set_active(BSP_GPIO_CH ch, bool active);
exit_code_t bsp_gpio_toggle_active(BSP_GPIO_CH ch);

exit_code_t bsp_gpio_irq_attach(BSP_GPIO_CH ch, bsp_gpio_irq_t edge, bsp_gpio_isr_handle isr, void *ctx);
exit_code_t bsp_gpio_irq_detach(BSP_GPIO_CH ch);