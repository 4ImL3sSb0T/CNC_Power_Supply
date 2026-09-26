#include "bsp_gpio.h"

static bsp_gpio_ch_t bsp_gpio_ch[] = {
    [BSP_GPIO_LED_STATUS] = {},
    [BSP_GPIO_KEY_MAIN] = {},
    [BSP_GPIO_KEY_AUX] = {},
    [BSP_GPIO_KEY_UP] = {},
    [BSP_GPIO_KEY_DOWN] = {}
};

static void bsp_gpio_irq_callback(uint gpio, uint32_t events) {
    for (u32 i = 0; i < count_of(bsp_gpio_ch); i++) {
        if (bsp_gpio_ch[i].gpio != gpio) continue;
        if (bsp_gpio_ch[i].isr) bsp_gpio_ch[i].isr((BSP_GPIO_CH)i, bsp_gpio_ch[i].ctx);
        return;
    }
  }

static bool bsp_gpio_valid(BSP_GPIO_CH ch) {
    return (u32)ch < count_of(bsp_gpio_ch);
}

static u32 bsp_gpio_irq_events(bsp_gpio_irq_t irq) {
    switch (irq) {
        case BSP_GPIO_IRQ_RISING:  return GPIO_IRQ_EDGE_RISE;
        case BSP_GPIO_IRQ_FALLING: return GPIO_IRQ_EDGE_FALL;
        case BSP_GPIO_IRQ_BOTH:    return GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL;
        default:                   return 0;
    }
}

exit_code_t bsp_gpio_init() {
    for (u32 index = 0; index < count_of(bsp_gpio_ch); index++) {
        const bsp_gpio_ch_t* cfg = &bsp_gpio_ch[index];

        gpio_init(cfg->gpio);
        if (cfg->dir == BSP_GPIO_DIR_OUT) gpio_put(cfg->gpio, cfg->init_active ^ cfg->active_low);

        gpio_set_function(cfg->gpio, GPIO_FUNC_SIO);

        switch (cfg->pull) {
            case BSP_GPIO_PULL_UP:   gpio_pull_up(cfg->gpio);      break;
            case BSP_GPIO_PULL_DOWN: gpio_pull_down(cfg->gpio);    break;
            case BSP_GPIO_PULL_NONE: gpio_disable_pulls(cfg->gpio); break;
        }

        gpio_set_dir(cfg->gpio, cfg->dir == BSP_GPIO_DIR_OUT);
    }
    return EXIT_OK;
}

bool bsp_gpio_get_active(BSP_GPIO_CH ch) {
    if (!bsp_gpio_valid(ch)) return false;
    const bsp_gpio_ch_t *cfg = &bsp_gpio_ch[ch];
    return gpio_get(cfg->gpio) ^ cfg->active_low;
}
exit_code_t bsp_gpio_set_active(BSP_GPIO_CH ch, bool active) {
    if (!bsp_gpio_valid(ch)) return EXIT_INVALID_PARAM;
    const bsp_gpio_ch_t *cfg = &bsp_gpio_ch[ch];
    if (cfg->dir != BSP_GPIO_DIR_OUT) return EXIT_NOT_SUPPORTED;
    gpio_put(cfg->gpio, (bool)(active ^ cfg->active_low));
    return EXIT_OK;
}
exit_code_t bsp_gpio_toggle_active(BSP_GPIO_CH ch) {
    return bsp_gpio_set_active(ch, !bsp_gpio_get_active(ch));
}

exit_code_t bsp_gpio_irq_attach(BSP_GPIO_CH ch, bsp_gpio_irq_t edge, bsp_gpio_isr_handle isr, void *ctx) {
    if (!bsp_gpio_valid(ch) || isr == NULL) return EXIT_INVALID_PARAM;

    const u32 events = bsp_gpio_irq_events(edge);
    if (events == 0) return EXIT_INVALID_PARAM;

    bsp_gpio_ch_t* cfg = &bsp_gpio_ch[ch];

    cfg->irq = edge;
    cfg->isr = isr;
    cfg->ctx = ctx;

    pio_set_irq_enabled_with_callback(cfg->gpio, events, true, bsp_gpio_irq_callback);
    return EXIT_OK;
}
exit_code_t bsp_gpio_irq_detach(BSP_GPIO_CH ch) {
    if (!bsp_gpio_valid(ch)) return EXIT_INVALID_PARAM;

    bsp_gpio_ch_t *cfg = &bsp_gpio_ch[ch];
    // 这里必须用普通版；用 with_callback(false, ...) 会把其他通道的中断也一起关掉
    gpio_set_irq_enabled(cfg->gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);

    cfg->irq = BSP_GPIO_IRQ_NONE;
    cfg->isr = NULL;
    cfg->ctx = NULL;
    return EXIT_OK;
}