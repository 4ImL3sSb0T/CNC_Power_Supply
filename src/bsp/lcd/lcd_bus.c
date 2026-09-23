/**
 * @file        lcd_bus.c
 * @brief       ST7789 SPI 命令/数据与 DMA 像素传输
 *
 * dma_channel_abort 在 SPI DREQ 饿死时会空等；spi_write_blocking 在
 * PL022 BSY/TNF 卡住时也不会返回。超时路径必须能自行恢复，不能把
 * async_context 主循环堵死。
 */

#include "bsp/lcd/lcd_priv.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#define LCD_DMA_TIMEOUT_US      50000u
#define LCD_SPI_TIMEOUT_US      5000u
#define LCD_ABORT_TIMEOUT_US    1000u

static int lcd_dma_tx = -1;
static int lcd_dma_rx = -1;
static volatile bool lcd_dma_busy;
static uint16_t lcd_dma_rx_dump;
static dma_channel_config lcd_tx_cfg;
static dma_channel_config lcd_rx_cfg;
static uint lcd_spi_bits = 8;

static void lcd_spi_drain(void)
{
    while (spi_is_readable(SPI_PORT)) {
        (void)spi_get_hw(SPI_PORT)->dr;
    }
    spi_get_hw(SPI_PORT)->icr = SPI_SSPICR_RORIC_BITS;
}

static bool lcd_spi_wait_not_busy(uint32_t timeout_us)
{
    uint32_t t0 = time_us_32();

    while (spi_get_hw(SPI_PORT)->sr & SPI_SSPSR_BSY_BITS) {
        if ((time_us_32() - t0) > timeout_us) {
            return false;
        }
        lcd_spi_drain();
        tight_loop_contents();
    }
    lcd_spi_drain();
    return true;
}

static void lcd_spi_recover(void)
{
    LCD_CS(1);
    spi_deinit(SPI_PORT);
    spi1_init();
    lcd_spi_bits = 8;
    lcd_dma_busy = false;
}

static void lcd_spi_set_bits(uint bits)
{
    if (lcd_spi_bits == bits) {
        return;
    }
    /* 改 DSS 前必须等移位结束；SSE=0 时 BSY 仍置位会锁死 PL022。 */
    if (!lcd_spi_wait_not_busy(LCD_SPI_TIMEOUT_US)) {
        lcd_spi_recover();
    }
    spi_set_format(SPI_PORT, bits, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    lcd_spi_bits = bits;
}

void lcd_spi_8bit(void)
{
    lcd_spi_set_bits(8);
}

void lcd_spi_16bit(void)
{
    lcd_spi_set_bits(16);
}

static void lcd_dma_abort_one(int ch)
{
    dma_channel_hw_t *hw;
    uint32_t t0;

    if (ch < 0) {
        return;
    }

    hw = dma_channel_hw_addr((uint)ch);
    /* DREQ 不再来时 abort 等不到 BUSY 清零；先改成无条件请求。 */
    hw_write_masked(&hw->al1_ctrl,
                    (uint32_t)DREQ_FORCE << DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB,
                    DMA_CH0_CTRL_TRIG_TREQ_SEL_BITS);
    /* RP2350-E5：abort 前清 EN，避免链式重触发。 */
    hw_clear_bits(&hw->al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_hw->abort = 1u << ch;

    t0 = time_us_32();
    while (hw->ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) {
        if ((time_us_32() - t0) > LCD_ABORT_TIMEOUT_US) {
            break;
        }
        tight_loop_contents();
    }
}

static void lcd_dma_reconfigure(void)
{
    if (lcd_dma_tx >= 0) {
        dma_channel_configure(lcd_dma_tx, &lcd_tx_cfg,
                              &spi_get_hw(SPI_PORT)->dr, NULL, 0, false);
    }
    if (lcd_dma_rx >= 0) {
        dma_channel_configure(lcd_dma_rx, &lcd_rx_cfg,
                              &lcd_dma_rx_dump, &spi_get_hw(SPI_PORT)->dr, 0, false);
    }
}

static void lcd_dma_abort(void)
{
    if (lcd_dma_rx >= 0) {
        dma_channel_set_irq0_enabled((uint)lcd_dma_rx, false);
    }
    lcd_dma_abort_one(lcd_dma_tx);
    lcd_dma_abort_one(lcd_dma_rx);
    if (lcd_dma_rx >= 0) {
        dma_channel_acknowledge_irq0((uint)lcd_dma_rx);
        dma_channel_set_irq0_enabled((uint)lcd_dma_rx, true);
    }
    lcd_dma_reconfigure();
    lcd_spi_recover();
}

void lcd_wait_idle(void)
{
    uint32_t t0 = time_us_32();

    while (lcd_dma_busy) {
        if ((time_us_32() - t0) > LCD_DMA_TIMEOUT_US) {
            lcd_dma_abort();
            break;
        }
        tight_loop_contents();
    }
    if (!lcd_spi_wait_not_busy(LCD_SPI_TIMEOUT_US)) {
        lcd_spi_recover();
    }
    lcd_spi_8bit();
}

bool lcd_is_busy(void)
{
    return lcd_dma_busy;
}

static void lcd_dma_irq_handler(void)
{
    if (lcd_dma_rx < 0 || !dma_channel_get_irq0_status((uint)lcd_dma_rx)) {
        return;
    }
    dma_channel_acknowledge_irq0((uint)lcd_dma_rx);
    LCD_CS(1);
    lcd_dma_busy = false;
}

void lcd_bus_init(void)
{
    if (lcd_dma_tx >= 0) {
        return;
    }

    lcd_dma_tx = dma_claim_unused_channel(true);
    lcd_dma_rx = dma_claim_unused_channel(true);

    lcd_tx_cfg = dma_channel_get_default_config((uint)lcd_dma_tx);
    channel_config_set_transfer_data_size(&lcd_tx_cfg, DMA_SIZE_16);
    channel_config_set_dreq(&lcd_tx_cfg, spi_get_dreq(SPI_PORT, true));
    channel_config_set_read_increment(&lcd_tx_cfg, true);
    channel_config_set_write_increment(&lcd_tx_cfg, false);

    lcd_rx_cfg = dma_channel_get_default_config((uint)lcd_dma_rx);
    channel_config_set_transfer_data_size(&lcd_rx_cfg, DMA_SIZE_16);
    channel_config_set_dreq(&lcd_rx_cfg, spi_get_dreq(SPI_PORT, false));
    channel_config_set_read_increment(&lcd_rx_cfg, false);
    channel_config_set_write_increment(&lcd_rx_cfg, false);

    lcd_dma_reconfigure();

    dma_channel_set_irq0_enabled((uint)lcd_dma_rx, true);
    irq_add_shared_handler(DMA_IRQ_0, lcd_dma_irq_handler,
                           PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);
}

void lcd_dma_start(const uint16_t *src, size_t count)
{
    uint32_t n;

    if (src == NULL || count == 0) {
        return;
    }

    lcd_spi_drain();

    n = dma_encode_transfer_count((uint)count);
    lcd_dma_busy = true;
    dma_channel_set_read_addr((uint)lcd_dma_tx, src, false);
    dma_channel_set_trans_count((uint)lcd_dma_tx, n, false);
    dma_channel_set_trans_count((uint)lcd_dma_rx, n, false);
    dma_start_channel_mask((1u << lcd_dma_tx) | (1u << lcd_dma_rx));
}

static bool lcd_spi_write_bytes(const uint8_t *data, size_t len)
{
    uint32_t t0 = time_us_32();
    size_t i;

    lcd_spi_8bit();
    for (i = 0; i < len; i++) {
        while (!spi_is_writable(SPI_PORT)) {
            if ((time_us_32() - t0) > LCD_SPI_TIMEOUT_US) {
                lcd_spi_recover();
                return false;
            }
            tight_loop_contents();
        }
        spi_get_hw(SPI_PORT)->dr = data[i];
    }
    if (!lcd_spi_wait_not_busy(LCD_SPI_TIMEOUT_US)) {
        lcd_spi_recover();
        return false;
    }
    return true;
}

void lcd_write_cmd(uint8_t cmd)
{
    lcd_wait_idle();
    LCD_DC(0);
    LCD_CS(0);
    (void)lcd_spi_write_bytes(&cmd, 1);
    LCD_CS(1);
}

void lcd_write_data(const uint8_t *data, int len)
{
    lcd_wait_idle();
    if (data == NULL || len <= 0) {
        return;
    }
    LCD_DC(1);
    LCD_CS(0);
    (void)lcd_spi_write_bytes(data, (size_t)len);
    LCD_CS(1);
}
