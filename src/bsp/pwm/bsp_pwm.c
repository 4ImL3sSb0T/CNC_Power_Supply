#include "bsp_pwm.h"

#include "FreeRTOS.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "semphr.h"

// wrap 和 level 都是 16 位寄存器。要让 duty = 1.0 输出恒高，需要 level > wrap，
// 即 level = wrap + 1；wrap 取 65535 时 wrap + 1 装不下，所以上限压到 65534。
#define BSP_PWM_WRAP_MAX     65534u

// clkdiv 是 8.4 定点数：16 表示 1.0，4095 表示 255.9375。
#define BSP_PWM_CLKDIV4_ONE  16u
#define BSP_PWM_CLKDIV4_MAX  4095u

#define BSP_PWM_LOCK_TIMEOUT_MS 1000u

// RP2350 有 12 个 PWM slice（RP2040 是 8 个）。用 sizeof 取而不是写死数字。
#define BSP_PWM_SLICE_COUNT  (sizeof(pwm_hw->slice) / sizeof(pwm_hw->slice[0]))

// 引脚分配见 docs/pin_map.md。四个频率互不相同的用途各占一个 slice；
// 功率级两路必须同频，所以共用一个 slice 的两个通道。
// 注意 GPIO16/17 虽然也是 PWM0 的 A/B，但已被 I2C0（IMU）占用，不能动。
static const bsp_pwm_ch_t bsp_pwm_ch[] = {
    [BSP_PWM_FAN] = {
        .pin = 18, .freq_hz = 25000, .init_duty = 0.0f, .phase_correct = false,
    },
    [BSP_PWM_BACKLIGHT] = {
        .pin = 20, .freq_hz = 20000, .init_duty = 1.0f, .phase_correct = false,
    },
    [BSP_PWM_SET_VOLTAGE] = {
        // 20 kHz 下约 12.9 位分辨率（3.3V 域约 0.8 mV 步进）。若更看重设定值的
        // 纹波而非分辨率，把 phase_correct 改成 true：纹波频率翻到 40 kHz 且没有
        // 偶次谐波，更好滤，代价是分辨率降到 11.9 位。
        .pin = 22, .freq_hz = 20000, .init_duty = 0.0f, .phase_correct = false,
    },
    [BSP_PWM_SET_CURRENT] = {
        // 频率和 phase_correct 必须与 SET_VOLTAGE 一致，否则 bsp_pwm_init 会拒绝
        .pin = 23, .freq_hz = 20000, .init_duty = 0.0f, .phase_correct = false,
    },
    [BSP_PWM_BUZZER] = {
        .pin = 24, .freq_hz = 2000, .init_duty = 0.0f, .phase_correct = false,
    },
};

typedef struct {
    const bsp_pwm_ch_t *cfg;
    u8    slice;
    u8    chan;
    u16   wrap;
    u8    clkdiv_int;
    u8    clkdiv_frac4;
    float duty;      // 目标有效量，关断期间也保留，供重新使能时恢复
    bool  enabled;
} bsp_pwm_ch_rt_t;

static bsp_pwm_ch_rt_t bsp_pwm_rt[count_of(bsp_pwm_ch)];
static bool bsp_pwm_inited = false;

// set_freq 是"关 slice → 改 wrap/clkdiv → 重算 level → 开 slice"的多步操作，
// 期间被打断会输出异常占空比，所以每个 slice 一把锁。set_duty / enable 只写
// 单个 level 寄存器，天然原子，不需要拿锁。
static SemaphoreHandle_t bsp_pwm_slice_mutex[BSP_PWM_SLICE_COUNT];

static bool bsp_pwm_valid(BSP_PWM_CH ch) {
    return (u32)ch < count_of(bsp_pwm_ch);
}

// 由目标频率反解 wrap 和 clkdiv。
//
//   f_pwm = f_clk / (clkdiv * (wrap + 1))
//
// 分辨率 = log2(wrap + 1)，所以要让 wrap 尽量大，即 clkdiv 取能满足 wrap 上限的
// 最小值。clkdiv 用 8.4 定点表示，向上取整保证 wrap 不超上限。
static exit_code_t bsp_pwm_solve(u32 freq_hz, bool phase_correct,
                                 u16 *wrap, u8 *div_int, u8 *div_frac4) {
    if (freq_hz == 0) return EXIT_INVALID_PARAM;

    const u64 f_clk = clock_get_hz(clk_sys);
    // 中心对齐模式下计数器正着走一遍再倒着走一遍，一个 PWM 周期占两倍时钟
    const u64 freq = (u64)freq_hz * (phase_correct ? 2u : 1u);

    // clkdiv >= f_clk / (freq * (WRAP_MAX + 1))
    const u64 denom = freq * (BSP_PWM_WRAP_MAX + 1u);
    u64 clkdiv4 = (f_clk * BSP_PWM_CLKDIV4_ONE + denom - 1u) / denom;

    if (clkdiv4 < BSP_PWM_CLKDIV4_ONE) clkdiv4 = BSP_PWM_CLKDIV4_ONE;
    if (clkdiv4 > BSP_PWM_CLKDIV4_MAX) return EXIT_INVALID_PARAM;  // 频率太低

    const u64 cycles = f_clk * BSP_PWM_CLKDIV4_ONE / (clkdiv4 * freq);
    if (cycles < 2u) return EXIT_INVALID_PARAM;  // 频率太高，连两个计数点都没有

    // clkdiv4 向上取整保证了 cycles <= WRAP_MAX + 1
    *wrap = (u16)(cycles - 1u);
    *div_int = (u8)(clkdiv4 / BSP_PWM_CLKDIV4_ONE);
    *div_frac4 = (u8)(clkdiv4 % BSP_PWM_CLKDIV4_ONE);
    return EXIT_OK;
}

// duty 是"有效量"：invert 由硬件输出极性负责，这里始终按 duty 写 level，
// 即 level 对应引脚高电平占比（invert 时引脚实际占空比是 1 - duty）。
//   0.0 -> level 0   -> 计数器永远不小于 0，输出恒低
//   1.0 -> level wrap + 1 -> 计数器永远小于 level，输出恒高
static u16 bsp_pwm_duty_to_level(const bsp_pwm_ch_rt_t *rt, float duty) {
    if (duty <= 0.0f) return 0;
    if (duty >= 1.0f) return (u16)(rt->wrap + 1u);
    return (u16)(duty * (float)(rt->wrap + 1u) + 0.5f);
}

exit_code_t bsp_pwm_init(void) {
    static bool inited = false;
    if (inited) return EXIT_ALREADY_INITIALIZED;

    // ---- 第一遍：求解分频，并把配置错误挡在这里 ----
    for (u32 i = 0; i < count_of(bsp_pwm_ch); i++) {
        bsp_pwm_ch_rt_t *rt = &bsp_pwm_rt[i];
        const bsp_pwm_ch_t *cfg = &bsp_pwm_ch[i];

        rt->cfg = cfg;
        rt->slice = (u8)pwm_gpio_to_slice_num(cfg->pin);
        rt->chan = (u8)pwm_gpio_to_channel(cfg->pin);

        exit_code_t rc = bsp_pwm_solve(cfg->freq_hz, cfg->phase_correct,
                                       &rt->wrap, &rt->clkdiv_int, &rt->clkdiv_frac4);
        if (rc != EXIT_OK) return rc;

        for (u32 j = 0; j < i; j++) {
            const bsp_pwm_ch_rt_t *prev = &bsp_pwm_rt[j];
            if (prev->slice != rt->slice) continue;

            // 同一 slice 的两个通道共用 wrap / clkdiv / 相位模式，频率必须一致
            if (prev->wrap != rt->wrap ||
                prev->clkdiv_int != rt->clkdiv_int ||
                prev->clkdiv_frac4 != rt->clkdiv_frac4 ||
                prev->cfg->phase_correct != cfg->phase_correct) {
                return EXIT_INVALID_PARAM;
            }
            // 两路不能映射到同一个通道（比如两个引脚都落在 slice 的 A）
            if (prev->chan == rt->chan) return EXIT_INVALID_PARAM;
        }

        rt->duty = cfg->init_duty;
        rt->enabled = true;
    }

    // ---- 第二遍：按 slice 配置硬件 ----
    bool slice_done[BSP_PWM_SLICE_COUNT] = {false};

    for (u32 i = 0; i < count_of(bsp_pwm_ch); i++) {
        bsp_pwm_ch_rt_t *rt = &bsp_pwm_rt[i];
        if (slice_done[rt->slice]) continue;
        slice_done[rt->slice] = true;

        // clkdiv / wrap / 相位模式 / 输出极性都是 slice 级的，所以先把该 slice
        // 上所有通道的引脚和极性收集齐，再一次性配置
        bool inv_a = false, inv_b = false;
        for (u32 j = 0; j < count_of(bsp_pwm_ch); j++) {
            const bsp_pwm_ch_rt_t *peer = &bsp_pwm_rt[j];
            if (peer->slice != rt->slice) continue;

            gpio_set_function(peer->cfg->pin, GPIO_FUNC_PWM);
            if (peer->chan == PWM_CHAN_A) inv_a = peer->cfg->invert;
            else                          inv_b = peer->cfg->invert;
        }

        pwm_config cfg = pwm_get_default_config();
        pwm_config_set_clkdiv_int_frac(&cfg, rt->clkdiv_int, rt->clkdiv_frac4);
        pwm_config_set_wrap(&cfg, rt->wrap);
        pwm_config_set_phase_correct(&cfg, rt->cfg->phase_correct);
        pwm_config_set_output_polarity(&cfg, inv_a, inv_b);

        // start = false：等所有通道的 level 都写好了再开 slice。否则从复位值 0
        // 到目标占空比之间会输出一个过渡周期，对功率级设定值就是上电毛刺
        pwm_init(rt->slice, &cfg, false);

        for (u32 j = 0; j < count_of(bsp_pwm_ch); j++) {
            const bsp_pwm_ch_rt_t *peer = &bsp_pwm_rt[j];
            if (peer->slice != rt->slice) continue;
            pwm_set_chan_level(rt->slice, peer->chan,
                               bsp_pwm_duty_to_level(peer, peer->duty));
        }

        pwm_set_enabled(rt->slice, true);
    }

    // ---- 每个用到的 slice 一把锁 ----
    for (u32 s = 0; s < BSP_PWM_SLICE_COUNT; s++) {
        if (!slice_done[s]) continue;
        bsp_pwm_slice_mutex[s] = xSemaphoreCreateMutex();
        if (bsp_pwm_slice_mutex[s] == NULL) return EXIT_NO_MEMORY;
    }

    bsp_pwm_inited = true;
    inited = true;
    return EXIT_OK;
}

exit_code_t bsp_pwm_set_duty(BSP_PWM_CH ch, float duty) {
    if (!bsp_pwm_valid(ch)) return EXIT_INVALID_PARAM;
    // 写成 !(a && b) 是为了把 NaN 也一起挡掉
    if (!(duty >= 0.0f && duty <= 1.0f)) return EXIT_INVALID_PARAM;

    bsp_pwm_ch_rt_t *rt = &bsp_pwm_rt[ch];

    // 单寄存器写，天然原子，不用拿 slice 锁
    if (rt->enabled) {
        pwm_set_chan_level(rt->slice, rt->chan, bsp_pwm_duty_to_level(rt, duty));
    }
    rt->duty = duty;
    return EXIT_OK;
}

exit_code_t bsp_pwm_get_duty(BSP_PWM_CH ch, float *duty) {
    if (!bsp_pwm_valid(ch) || duty == NULL) return EXIT_INVALID_PARAM;
    *duty = bsp_pwm_rt[ch].duty;
    return EXIT_OK;
}

// 换频率。wrap 和 clkdiv 是 slice 共享的，所以这里动的是整个 slice：
// 同一 slice 上的所有通道都要按各自的 duty 重新算 level。
static exit_code_t bsp_pwm_apply_freq(bsp_pwm_ch_rt_t *rt, u32 freq_hz) {
    u16 wrap;
    u8 div_int, div_frac4;
    exit_code_t rc = bsp_pwm_solve(freq_hz, rt->cfg->phase_correct,
                                   &wrap, &div_int, &div_frac4);
    if (rc != EXIT_OK) return rc;

    // 分频量化后可能落回同一个值，那就没必要扰动 slice
    if (wrap == rt->wrap &&
        div_int == rt->clkdiv_int &&
        div_frac4 == rt->clkdiv_frac4) {
        return EXIT_OK;
    }

    SemaphoreHandle_t lock = bsp_pwm_slice_mutex[rt->slice];
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(BSP_PWM_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return EXIT_TIMEOUT;
    }

    pwm_set_enabled(rt->slice, false);
    pwm_set_clkdiv_int_frac(rt->slice, div_int, div_frac4);
    pwm_set_wrap(rt->slice, wrap);

    for (u32 j = 0; j < count_of(bsp_pwm_ch); j++) {
        bsp_pwm_ch_rt_t *peer = &bsp_pwm_rt[j];
        if (peer->slice != rt->slice) continue;

        peer->wrap = wrap;
        peer->clkdiv_int = div_int;
        peer->clkdiv_frac4 = div_frac4;
        pwm_set_chan_level(rt->slice, peer->chan,
                           peer->enabled ? bsp_pwm_duty_to_level(peer, peer->duty) : 0);
    }

    pwm_set_enabled(rt->slice, true);

    xSemaphoreGive(lock);
    return EXIT_OK;
}

exit_code_t bsp_pwm_set_freq(BSP_PWM_CH ch, u32 freq_hz) {
    if (!bsp_pwm_valid(ch)) return EXIT_INVALID_PARAM;
    return bsp_pwm_apply_freq(&bsp_pwm_rt[ch], freq_hz);
}

exit_code_t bsp_pwm_set_freq_duty(BSP_PWM_CH ch, u32 freq_hz, float duty) {
    if (!bsp_pwm_valid(ch)) return EXIT_INVALID_PARAM;
    if (!(duty >= 0.0f && duty <= 1.0f)) return EXIT_INVALID_PARAM;

    bsp_pwm_ch_rt_t *rt = &bsp_pwm_rt[ch];
    rt->duty = duty;

    exit_code_t rc = bsp_pwm_apply_freq(rt, freq_hz);
    if (rc != EXIT_OK) return rc;

    // apply_freq 在频率量化后没变时会提前返回，所以这里补一次，保证 duty 生效
    if (rt->enabled) {
        pwm_set_chan_level(rt->slice, rt->chan, bsp_pwm_duty_to_level(rt, duty));
    }
    return EXIT_OK;
}

exit_code_t bsp_pwm_enable(BSP_PWM_CH ch, bool enable) {
    if (!bsp_pwm_valid(ch)) return EXIT_INVALID_PARAM;

    bsp_pwm_ch_rt_t *rt = &bsp_pwm_rt[ch];
    rt->enabled = enable;

    // 关断用 level = 0 实现，而不是 pwm_set_enabled(slice, false)——后者会把
    // 同一 slice 上另一路也一起关掉
    pwm_set_chan_level(rt->slice, rt->chan,
                       enable ? bsp_pwm_duty_to_level(rt, rt->duty) : 0);
    return EXIT_OK;
}

exit_code_t bsp_pwm_get_info(BSP_PWM_CH ch, bsp_pwm_info_t *info) {
    if (!bsp_pwm_valid(ch) || info == NULL) return EXIT_INVALID_PARAM;

    const bsp_pwm_ch_rt_t *rt = &bsp_pwm_rt[ch];
    const u64 clkdiv4 = (u64)rt->clkdiv_int * BSP_PWM_CLKDIV4_ONE + rt->clkdiv_frac4;
    const u64 cycles = (u64)(rt->wrap + 1u) * (rt->cfg->phase_correct ? 2u : 1u);

    info->actual_freq_hz = (u32)((u64)clock_get_hz(clk_sys) * BSP_PWM_CLKDIV4_ONE
                                 / clkdiv4 / cycles);
    info->wrap = rt->wrap;
    info->clkdiv_int = rt->clkdiv_int;
    info->clkdiv_frac4 = rt->clkdiv_frac4;
    info->duty = rt->duty;
    info->enabled = rt->enabled;
    return EXIT_OK;
}
