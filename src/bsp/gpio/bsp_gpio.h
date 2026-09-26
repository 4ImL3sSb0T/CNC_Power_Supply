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

/// @brief 初始化所有通道的引脚。
///        必须先于 irq_attach / irq_detach 调用——临界区用的硬件自旋锁在这里申请，
///        没初始化就调那两处会返回 EXIT_NOT_INITIALIZED。
exit_code_t bsp_gpio_init();

bool bsp_gpio_get_active(BSP_GPIO_CH ch);
exit_code_t bsp_gpio_set_active(BSP_GPIO_CH ch, bool active);
exit_code_t bsp_gpio_toggle_active(BSP_GPIO_CH ch);

/// @brief 挂载 / 卸载引脚中断。
///
/// isr 在**中断上下文**执行，里面只能用 FreeRTOS 的 *FromISR 一族接口，
/// 不能调用任何可能阻塞的 API。
///
/// 注册表被临界区保护（关本核中断 + 硬件自旋锁），所以 attach / detach 彼此之间
/// 以及和本核的中断回调之间是排他的。但中断回调自己不拿这把锁——中断里拿不了——
/// 所以还有两处需要调用方注意：
///   - 另一核上正在执行的 in-flight 回调不会因为 detach 而停下，detach 返回时
///     若该回调正持有 ctx，此时释放 ctx 会造成 use-after-free；
///   - 中断回调被装在**首次调用 attach 的那个核**上，所以要严格避免上面这种情况，
///     请固定在同一核上调用 attach，或者不释放 ctx。
exit_code_t bsp_gpio_irq_attach(BSP_GPIO_CH ch, bsp_gpio_irq_t edge, bsp_gpio_isr_handle isr, void *ctx);
exit_code_t bsp_gpio_irq_detach(BSP_GPIO_CH ch);

