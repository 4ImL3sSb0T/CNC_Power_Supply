/**
 * @file        pcb_ctrl.c
 * @brief       SC8701 控制脚驱动：PWM2 双通道 50kHz + CE# + PG 节点 ADC
 *
 * 初始化顺序的安全要求（SC8701 /CE 内部 1M 下拉 = 悬空即上电使能；
 * IPWM 悬空芯片不能正常工作，D=0 时限流为 0）：
 *   1. CE# 先锁存"关断"电平再切输出，杜绝上电瞬间误使能；
 *   2. PWM/IPWM 脚先以 GPIO 输出低（0% = 安全），再改 PWM 复用。
 */

#include "bsp/power/pcb_ctrl.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"

#define PG_ADC_AVG_SAMPLES      8u

static u32 s_period;            /* 每周期计数 = wrap + 1 */
static u16 s_pwm_permille;
static u16 s_ipwm_permille;
static bool s_ce_on;
static pcb_pg_state_t s_pg_state = PG_UNKNOWN;

static void pwm_slice_setup(uint slice)
{
    pwm_config cfg = pwm_get_default_config();

    pwm_config_set_clkdiv_int(&cfg, 1);
    pwm_config_set_wrap(&cfg, (u16)(s_period - 1u));
    pwm_init(slice, &cfg, true);
}

exit_code_t pcb_ctrl_init(void)
{
    u32 sys_hz;
    u32 period;

    /* 1) CE#：先锁存关断电平，再开输出方向 */
    gpio_init(PCB_PIN_CE_N);
    gpio_put(PCB_PIN_CE_N, 1);
    gpio_set_dir(PCB_PIN_CE_N, GPIO_OUT);

    /* 2) PWM/IPWM：先以 GPIO 驱动低电平（占空比 0% 语义），消除悬空窗口 */
    gpio_init(PCB_PIN_PWM);
    gpio_put(PCB_PIN_PWM, 0);
    gpio_set_dir(PCB_PIN_PWM, GPIO_OUT);
    gpio_init(PCB_PIN_IPWM);
    gpio_put(PCB_PIN_IPWM, 0);
    gpio_set_dir(PCB_PIN_IPWM, GPIO_OUT);

    s_pwm_permille = 0;
    s_ipwm_permille = 0;
    s_ce_on = false;

    /* 3) PWM 输出：50kHz（SC8701 接受 20–100kHz） */
    sys_hz = clock_get_hz(clk_sys);
    period = sys_hz / PCB_PWM_FREQ_HZ;
    if (period < 2u) {
        period = 2u;
    }
    if (period > 65536u) {
        period = 65536u;
    }
    s_period = period;

    pwm_slice_setup(pwm_gpio_to_slice_num(PCB_PIN_PWM));
    if (pwm_gpio_to_slice_num(PCB_PIN_IPWM) != pwm_gpio_to_slice_num(PCB_PIN_PWM)) {
        /* 两脚不在同一 slice 时各自初始化（同频同 wrap） */
        pwm_slice_setup(pwm_gpio_to_slice_num(PCB_PIN_IPWM));
    }
    gpio_set_function(PCB_PIN_PWM, GPIO_FUNC_PWM);
    gpio_set_function(PCB_PIN_IPWM, GPIO_FUNC_PWM);
    pwm_set_gpio_level(PCB_PIN_PWM, 0);
    pwm_set_gpio_level(PCB_PIN_IPWM, 0);

    /* 4) ADC：PG 节点。adc_gpio_init 会关掉数字输入缓冲（RP2350-E9 需要） */
    adc_init();
    adc_gpio_init(PCB_ADC_PG_GPIO);
#if PCB_TEST_VOUT_SENSE_ENABLE
    adc_gpio_init(PCB_ADC_VOUT_GPIO);
#endif

    return EXIT_OK;
}

void pcb_ctrl_safe_state(void)
{
    pcb_ctrl_set_pwm_duty(0);
    pcb_ctrl_set_ipwm_duty(0);
    pcb_ctrl_ce(false);
}

void pcb_ctrl_ce(bool enable)
{
    /* CE# 低有效 */
    gpio_put(PCB_PIN_CE_N, enable ? 0 : 1);
    s_ce_on = enable;
}

bool pcb_ctrl_ce_on(void)
{
    return s_ce_on;
}

void pcb_ctrl_set_pwm_duty(u16 permille)
{
    if (permille > 1000u) {
        permille = 1000u;
    }
    s_pwm_permille = permille;
    pwm_set_gpio_level(PCB_PIN_PWM, (u16)((s_period * permille) / 1000u));
}

void pcb_ctrl_set_ipwm_duty(u16 permille)
{
    if (permille > 1000u) {
        permille = 1000u;
    }
    s_ipwm_permille = permille;
    pwm_set_gpio_level(PCB_PIN_IPWM, (u16)((s_period * permille) / 1000u));
}

u16 pcb_ctrl_pwm_duty(void)
{
    return s_pwm_permille;
}

u16 pcb_ctrl_ipwm_duty(void)
{
    return s_ipwm_permille;
}

u16 pcb_ctrl_vout_setpoint_mv(void)
{
    /* VSET = (1 + 5D)/6 × 23.4V，D = permille/1000 ⇒ mV = 23400×(1000+5p)/6000 */
    u32 p = s_pwm_permille;

    return (u16)((PCB_VOUT_SET_MV * (1000u + 5u * p)) / 6000u);
}

u16 pcb_ctrl_ilim2_setpoint_ma(void)
{
    return (u16)((PCB_ILIM2_FULL_MA * s_ipwm_permille) / 1000u);
}

u16 pcb_ctrl_pg_node_mv(void)
{
    u32 sum = 0;
    u32 i;

    adc_select_input(PCB_ADC_PG_CH);
    for (i = 0; i < PG_ADC_AVG_SAMPLES; i++) {
        sum += adc_read();
    }
    return (u16)(((sum / PG_ADC_AVG_SAMPLES) * 3300u) / 4096u);
}

pcb_pg_state_t pcb_ctrl_pg_state(void)
{
    u16 mv = pcb_ctrl_pg_node_mv();

    if (mv >= PCB_PG_GOOD_MIN_MV) {
        s_pg_state = PG_GOOD;
    } else if (mv <= PCB_PG_FAULT_MAX_MV) {
        s_pg_state = PG_FAULT;
    }
    /* 滞回区间内保持上次判定 */
    return s_pg_state;
}

#if PCB_TEST_VOUT_SENSE_ENABLE
u16 pcb_ctrl_read_vout_mv(void)
{
    u32 sum = 0;
    u32 i;

    adc_select_input(PCB_ADC_VOUT_CH);
    for (i = 0; i < PG_ADC_AVG_SAMPLES; i++) {
        sum += adc_read();
    }
    /* 12-bit SAR，Vref=3.3V；再乘分压比（×1000 表示，先乘后除防失精度） */
    return (u16)((((sum / PG_ADC_AVG_SAMPLES) * 3300u) / 4096u) * PCB_VOUT_SENSE_DIV_X1000 / 1000u);
}
#endif
