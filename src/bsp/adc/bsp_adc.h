#pragma once
#include "hardware/adc.h"
#include "lib/tools/common_def.h"
#include "stdint.h"

// 唐完了, 只有三个ADC通道可以使用, 那就两个检测电压, 一个检测PG吧, 电流使用ADC芯片检测吧
typedef enum {
    BSP_ADC_SUPPLY_INPUT_VOLTAGE,
    // BSP_ADC_INPUT_CURRENT,
    BSP_ADC_SUPPLY_OUTPUT_VOLTAGE,
    // BSP_ADC_OUTPUT_CURRENT,
    BSP_ADC_SUPPLY_PG,
    BSP_ADC_MCU_TEMP
} BSP_ADC_CH;

typedef struct {
    u32 pin;
    float factor;
    u16 raw_value;
} bsp_adc_ch_t;

exit_code_t bsp_adc_init();

/// @brief 获得最新一次的ADC
/// @param ch 通道
/// @return Raw Value
u16 bsp_adc_get_raw_value(BSP_ADC_CH ch);


/// @brief 获得平均的ADC
/// @param ch 通道
/// @return 平均值
float bsp_adc_get_value(BSP_ADC_CH ch);
