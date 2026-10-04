#include <assert.h>
#include <stdio.h>
#include "../../../src/bsp/adc/bsp_adc.c"

static void check_voltage_channels(void) {
    assert(bsp_adc_get_raw_value(BSP_ADC_SUPPLY_INPUT_VOLTAGE) == 1111);
    assert(bsp_adc_get_raw_value(BSP_ADC_SUPPLY_OUTPUT_VOLTAGE) == 2222);
    assert(mock_adc_round_robin == 3);
}
int main(void) {
    assert(bsp_adc_init() == EXIT_OK);
    check_voltage_channels();
    for (unsigned i = 0; i < 3; i++) {
        assert(bsp_adc_get_raw_value(BSP_ADC_SUPPLY_PG) == 3333);
        assert(mock_adc_last_input == 2);
        check_voltage_channels();
        assert(bsp_adc_get_raw_value(BSP_ADC_MCU_TEMP) == 900);
        assert(mock_adc_last_input == ADC_TEMPERATURE_CHANNEL_NUM);
        check_voltage_channels();
    }
    puts("PASS: ADC temperature mapping and FIFO/channel sequence after restart");
    return 0;
}
