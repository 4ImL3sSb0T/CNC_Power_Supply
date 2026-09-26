#include "bsp_gpio.h"

#include "pico/critical_section.h"

static bsp_gpio_ch_t bsp_gpio_ch[] = {
    [BSP_GPIO_LED_STATUS] = {},
    [BSP_GPIO_KEY_MAIN] = {},
    [BSP_GPIO_KEY_AUX] = {},
    [BSP_GPIO_KEY_UP] = {},
    [BSP_GPIO_KEY_DOWN] = {}
};

// attach / detach 在任务上下文写注册表，bsp_gpio_irq_callback 在中断上下文读它。
// 这里不能用信号量——mutex 不允许在中断里获取——所以用临界区：进临界区会关掉
// 本核中断并拿到硬件自旋锁，对本核中断和另一核的临界区都是排它的。
static critical_section_t bsp_gpio_irq_lock;
static bool bsp_gpio_inited = false;

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
    if (!bsp_gpio_inited) {
        // critical_section_init 每次调用都会占用一把新的硬件自旋锁，所以只能来一次
        critical_section_init(&bsp_gpio_irq_lock);
        bsp_gpio_inited = true;
    }

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
    if (!bsp_gpio_valid(ch)) return EXIT_INVALID_PARAM;
    const bsp_gpio_ch_t *cfg = &bsp_gpio_ch[ch];
    if (cfg->dir != BSP_GPIO_DIR_OUT) return EXIT_NOT_SUPPORTED;

    // 单次写 GPIO_OUT_XOR，取反在硬件里完成，所以并发的翻转不会互相丢。
    // 用 64 位版本是因为 RP2350 有 GPIO32 以上的引脚，gpio_xor_mask 只覆盖低 32 位。
    gpio_xor_mask64(1ull << cfg->gpio);
    return EXIT_OK;
}

exit_code_t bsp_gpio_irq_attach(BSP_GPIO_CH ch, bsp_gpio_irq_t edge, bsp_gpio_isr_handle isr, void *ctx) {
    if (!bsp_gpio_inited) return EXIT_NOT_INITIALIZED;
    if (!bsp_gpio_valid(ch) || isr == NULL) return EXIT_INVALID_PARAM;

    const u32 events = bsp_gpio_irq_events(edge);
    if (events == 0) return EXIT_INVALID_PARAM;

    bsp_gpio_ch_t* cfg = &bsp_gpio_ch[ch];

    critical_section_enter_blocking(&bsp_gpio_irq_lock);
    // 注册项三件套和使能中断是一件事：中断一旦使能就可能立刻进来，不能让它读到
    // 只写了一半的注册项。顺序也不能反——回调只认已填好的 isr/ctx。
    cfg->irq = edge;
    cfg->isr = isr;
    cfg->ctx = ctx;
    gpio_set_irq_enabled_with_callback(cfg->gpio, events, true, bsp_gpio_irq_callback);
    critical_section_exit(&bsp_gpio_irq_lock);
    return EXIT_OK;
}
exit_code_t bsp_gpio_irq_detach(BSP_GPIO_CH ch) {
    if (!bsp_gpio_inited) return EXIT_NOT_INITIALIZED;
    if (!bsp_gpio_valid(ch)) return EXIT_INVALID_PARAM;

    bsp_gpio_ch_t *cfg = &bsp_gpio_ch[ch];

    critical_section_enter_blocking(&bsp_gpio_irq_lock);
    // 先关中断再清注册项：反过来的话清完之后中断仍可能进来，读到一个空注册项。
    // 关中断这里必须用普通版；用 with_callback(false, ...) 会把其他通道的中断也一起关掉
    gpio_set_irq_enabled(cfg->gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);

    cfg->irq = BSP_GPIO_IRQ_NONE;
    cfg->isr = NULL;
    cfg->ctx = NULL;
    critical_section_exit(&bsp_gpio_irq_lock);
    return EXIT_OK;
}