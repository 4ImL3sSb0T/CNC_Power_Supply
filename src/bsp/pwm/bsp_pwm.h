#pragma once
#include "lib/tools/common_def.h"
#include "stdbool.h"
#include "stdint.h"

// 五路 PWM 通道。除两个功率级设定通道外，每路独占一个 PWM slice，
// 所以改任一路的频率都不会牵连其他路。
//
// 功率级两路（电压 / 电流设定）共用 PWM3 的 A/B 两个通道：它们本来就是
// 同一输出的两个环的基准，共用 slice 让硬件保证两路严格同频——若各自
// 独立调频，两个基准之间会出现拍频，反映到输出上就是低频纹波。
// 代价是这两路不能单独改频率，set_freq 改的是整个 slice。
typedef enum {
    BSP_PWM_FAN,          // 风扇调速
    BSP_PWM_BACKLIGHT,    // 屏幕背光调光
    BSP_PWM_SET_VOLTAGE,  // 功率级电压设定值（外部 RC 滤波后当 DAC 用）
    BSP_PWM_SET_CURRENT,  // 功率级电流设定值，与电压设定共用 slice
    BSP_PWM_BUZZER,       // 无源蜂鸣器，靠改频率出音调
} BSP_PWM_CH;

typedef struct {
    u32   pin;
    u32   freq_hz;        // 期望频率（实际值受 clkdiv 量化影响，见 bsp_pwm_get_info）
    float init_duty;      // 0.0 ~ 1.0
    bool  phase_correct;  // 中心对齐：纹波频率翻倍且无偶次谐波，更好滤，代价是分辨率少 1 位
    bool  invert;         // 外部驱动低有效时置 true，duty 仍表示有效量
} bsp_pwm_ch_t;

typedef struct {
    u32   actual_freq_hz;  // 量化后的实际频率
    u16   wrap;            // 计数器上限，分辨率 = log2(wrap + 1) 位
    u8    clkdiv_int;
    u8    clkdiv_frac4;    // clkdiv 的小数部分（8.4 定点）
    float duty;
    bool  enabled;
} bsp_pwm_info_t;

exit_code_t bsp_pwm_init(void);

/// @brief 设定占空比
/// @param duty 有效量，0.0 = 全关，1.0 = 全开
exit_code_t bsp_pwm_set_duty(BSP_PWM_CH ch, float duty);
exit_code_t bsp_pwm_get_duty(BSP_PWM_CH ch, float *duty);

/// @brief 改频率。会同时改同一 slice 上所有通道的频率（功率级两路绑在一起）。
///        内部按各通道当前 duty 重算 level，不会出现中间态的异常占空比。
exit_code_t bsp_pwm_set_freq(BSP_PWM_CH ch, u32 freq_hz);
exit_code_t bsp_pwm_set_freq_duty(BSP_PWM_CH ch, u32 freq_hz, float duty);

/// @brief 使能 / 关断单路
///        关断只是把该路输出压到"有效量为 0"的电平，不影响同一 slice 上的
///        其他通道；重新使能会恢复关断前的占空比。
exit_code_t bsp_pwm_enable(BSP_PWM_CH ch, bool enable);

/// @brief 读取该路的实际频率、分辨率、当前占空比
exit_code_t bsp_pwm_get_info(BSP_PWM_CH ch, bsp_pwm_info_t *info);
