#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../../../src/bsp/pwm/bsp_pwm.c"

static void check_output(BSP_PWM_CH ch, float duty, bool enabled) {
    bsp_pwm_info_t info;
    assert(bsp_pwm_get_info(ch, &info) == EXIT_OK);
    assert(fabsf(info.duty - duty) < 0.0001f && info.enabled == enabled);
    uint slice = pwm_gpio_to_slice_num(bsp_pwm_ch[ch].pin);
    uint channel = pwm_gpio_to_channel(bsp_pwm_ch[ch].pin);
    uint level = (pwm_hw->slice[slice].cc >> (16 * channel)) & 0xFFFF;
    uint expected = enabled ? (uint)(duty * (info.wrap + 1u) + 0.5f) : 0;
    assert(level == expected);
}
static void change_frequency_before_duty_lock(void) {
    assert(bsp_pwm_set_freq(BSP_PWM_SET_VOLTAGE, 10000) == EXIT_OK);
}

int main(void) {
    bsp_pwm_info_t info;
    assert(bsp_pwm_get_info(BSP_PWM_SET_VOLTAGE, &info) == EXIT_NOT_INITIALIZED);
    assert(bsp_pwm_init() == EXIT_OK);
    for (unsigned i = 0; i < count_of(bsp_pwm_ch); i++) {
        const uint slice = pwm_gpio_to_slice_num(bsp_pwm_ch[i].pin);
        mock_pwm_mutex[slice] = bsp_pwm_slice_mutex[slice];
    }
    assert(bsp_pwm_set_duty(BSP_PWM_SET_VOLTAGE, 0.25f) == EXIT_OK);
    assert(bsp_pwm_set_duty(BSP_PWM_SET_CURRENT, 0.6f) == EXIT_OK);
    check_output(BSP_PWM_SET_VOLTAGE, 0.25f, true);
    check_output(BSP_PWM_SET_CURRENT, 0.6f, true);

    // 模拟另一个任务在本次占空比更新拿锁前改频，必须按新 wrap 计算。
    mock_before_mutex_take = change_frequency_before_duty_lock;
    assert(bsp_pwm_set_duty(BSP_PWM_SET_VOLTAGE, 0.5f) == EXIT_OK);
    assert(mock_before_mutex_take == NULL);
    check_output(BSP_PWM_SET_VOLTAGE, 0.5f, true);
    check_output(BSP_PWM_SET_CURRENT, 0.6f, true);

    assert(bsp_pwm_set_freq_duty(BSP_PWM_SET_VOLTAGE, 0, 0.9f) == EXIT_INVALID_PARAM);
    check_output(BSP_PWM_SET_VOLTAGE, 0.5f, true);
    mock_fail_mutex = true;
    assert(bsp_pwm_set_freq_duty(BSP_PWM_SET_VOLTAGE, 20000, 0.9f) == EXIT_TIMEOUT);
    assert(bsp_pwm_set_duty(BSP_PWM_SET_VOLTAGE, 0.9f) == EXIT_TIMEOUT);
    assert(bsp_pwm_enable(BSP_PWM_SET_VOLTAGE, false) == EXIT_TIMEOUT);
    mock_fail_mutex = false;
    check_output(BSP_PWM_SET_VOLTAGE, 0.5f, true);

    assert(bsp_pwm_enable(BSP_PWM_SET_VOLTAGE, false) == EXIT_OK);
    assert(bsp_pwm_set_freq_duty(BSP_PWM_SET_VOLTAGE, 20000, 0.2f) == EXIT_OK);
    check_output(BSP_PWM_SET_VOLTAGE, 0.2f, false);
    check_output(BSP_PWM_SET_CURRENT, 0.6f, true);
    assert(bsp_pwm_enable(BSP_PWM_SET_VOLTAGE, true) == EXIT_OK);
    check_output(BSP_PWM_SET_VOLTAGE, 0.2f, true);
    assert(bsp_pwm_set_freq_duty(BSP_PWM_SET_VOLTAGE, 20000, 1.0f) == EXIT_OK);
    check_output(BSP_PWM_SET_VOLTAGE, 1.0f, true);
    assert(bsp_pwm_set_duty(BSP_PWM_SET_VOLTAGE, NAN) == EXIT_INVALID_PARAM);
    puts("PASS: PWM serialized updates, shared slice, timeout and rollback");
    return 0;
}
