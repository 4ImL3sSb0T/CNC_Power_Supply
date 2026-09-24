#include "bsp_adc.h"
#include "hardware/dma.h"

bsp_adc_ch_t bsp_adc_ch[] = {
    [BSP_ADC_SUPPLY_INPUT_VOLTAGE] = {.pin = 26, .factor = 1.0f},
    [BSP_ADC_SUPPLY_OUTPUT_VOLTAGE] = { .pin = 27, .factor = 11.0f},
    [BSP_ADC_SUPPLY_PG] = { .pin = 28, .factor = 1.0f},
    [BSP_ADC_MCU_TEMP] = {.factor = 1.0f}
};

exit_code_t bsp_adc_init() {
    adc_init();
    adc_gpio_init(bsp_adc_ch[BSP_ADC_SUPPLY_INPUT_VOLTAGE].pin);
    adc_gpio_init(bsp_adc_ch[BSP_ADC_SUPPLY_OUTPUT_VOLTAGE].pin);
    adc_gpio_init(bsp_adc_ch[BSP_ADC_SUPPLY_PG].pin);
    adc_set_temp_sensor_enabled(true);

    adc_fifo_setup(true, true, 1, false, false);
    adc_set_round_robin(0);


}

u16 bsp_adc_get_raw_value(BSP_ADC_CH ch) {

}
float bsp_adc_get_value(BSP_ADC_CH ch) {

}

float bsp_adc_get_mcu_temp();
