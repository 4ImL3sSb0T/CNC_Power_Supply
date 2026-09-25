#include "bsp_adc.h"
#include "hardware/dma.h"

#define BSP_ADC_BUF_WORDS  512u   // 必须是 2 的幂
#define BSP_ADC_RING_BITS  10u    // 512 * sizeof(u16) = 1024 字节 = 1 << 10
#define BSP_ADC_REF_VOLTAGE 3.3f
#define BSP_ADC_REF_RES 4096

static u16 bsp_adc_buf[BSP_ADC_BUF_WORDS]
__attribute__((aligned(1u << BSP_ADC_RING_BITS)));

static int adc_dam_ch;
static dma_channel_config cfg;

bsp_adc_ch_t bsp_adc_ch[] = {
    [BSP_ADC_SUPPLY_INPUT_VOLTAGE] = {.pin = 26, .factor = 1.0f},
    [BSP_ADC_SUPPLY_OUTPUT_VOLTAGE] = { .pin = 27, .factor = 11.0f},
    [BSP_ADC_SUPPLY_PG] = { .pin = 28, .factor = 1.0f},
    [BSP_ADC_MCU_TEMP] = {.factor = 1.0f}
};

static u32 bsp_adc_write_pos(void) {
    // 1. 获取 DMA 通道的硬件寄存器结构体指针，并读取其中的 write_addr 寄存器
    const uintptr_t wr = (uintptr_t)dma_channel_hw_addr(adc_dam_ch)->write_addr;
    const u32 off = (u32)(wr - (uintptr_t)bsp_adc_buf) & ((1u << BSP_ADC_RING_BITS) - 1);

    return off / sizeof(u16);;
}

static float bsp_adc_read_voltage(BSP_ADC_CH ch_off, float factor) {
    f32 sum = 0;
    const u32 n = BSP_ADC_BUF_WORDS / 2;   // 每个通道 256 个样本
    for (u32 i = 0; i < n; i++) {
        sum += bsp_adc_buf[2 * i + ch_off];
    }
    // 12-bit ADC，参考电压 3.3V
    return sum / n / BSP_ADC_REF_RES * BSP_ADC_REF_VOLTAGE * factor;
}

static u16 bsp_adc_read_voltage_stop_dma(BSP_ADC_CH ch) {
    u16 value = 0;
    switch (ch) {
    case BSP_ADC_SUPPLY_PG:
    case BSP_ADC_MCU_TEMP:
        adc_run(false);
        dma_channel_abort(bsp_adc_ch);
        adc_fifo_drain();
        adc_select_input(ch);
        value = adc_read();
        adc_select_input(0);
        dma_channel_configure(adc_dam_ch, &cfg, bsp_adc_buf, &adc_hw->fifo, dma_encode_endless_transfer_count(), true);
        adc_run(true);
        break;
    default:
        break;
    }
    return value;
}

exit_code_t bsp_adc_init() {
    adc_init();
    adc_gpio_init(bsp_adc_ch[BSP_ADC_SUPPLY_INPUT_VOLTAGE].pin);
    adc_gpio_init(bsp_adc_ch[BSP_ADC_SUPPLY_OUTPUT_VOLTAGE].pin);
    adc_gpio_init(bsp_adc_ch[BSP_ADC_SUPPLY_PG].pin);
    adc_set_temp_sensor_enabled(true);

    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv(0);
    adc_set_round_robin(0b11);

    adc_dam_ch = dma_claim_unused_channel(true);
    cfg = dma_channel_get_default_config(adc_dam_ch);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_ring(&cfg, true, BSP_ADC_RING_BITS);
    channel_config_set_dreq(&cfg, DREQ_ADC);

    dma_channel_configure(adc_dam_ch, &cfg, bsp_adc_buf, &adc_hw->fifo, dma_encode_endless_transfer_count(), true);
    adc_run(true);
}

u16 bsp_adc_get_raw_value(BSP_ADC_CH ch) {
    u16 raw_value = 0;
    switch (ch) {
    case BSP_ADC_SUPPLY_INPUT_VOLTAGE:
    case BSP_ADC_SUPPLY_OUTPUT_VOLTAGE:
        const u32 parity = (ch == BSP_ADC_SUPPLY_INPUT_VOLTAGE) ? 0u : 1u;
        u32 pos = bsp_adc_write_pos();
        // 退 2 格，并强制成目标通道的奇偶性，避开正在被 DMA 改写的槽
        pos = (pos + BSP_ADC_BUF_WORDS - 2u) & (BSP_ADC_BUF_WORDS - 1u);
        pos = (pos & ~1u) | parity;
        raw_value = bsp_adc_buf[pos];
    case BSP_ADC_SUPPLY_PG:
    case BSP_ADC_MCU_TEMP:
        raw_value = bsp_adc_read_voltage_stop_dma(ch);
        break;
    default:
        break;
    }
    return raw_value;
}

float bsp_adc_get_value(BSP_ADC_CH ch) {
    float voltage = 0.0f;
    switch (ch) {
    case BSP_ADC_SUPPLY_INPUT_VOLTAGE:
    case BSP_ADC_SUPPLY_OUTPUT_VOLTAGE:
        voltage = bsp_adc_read_voltage(ch, bsp_adc_ch[ch].factor);
        break;
    case BSP_ADC_SUPPLY_PG: 
    case BSP_ADC_MCU_TEMP:
        voltage = bsp_adc_read_voltage_stop_dma(ch) * bsp_adc_ch[ch].factor;
        break;
    default:
        break;
    }
    return voltage;
}
