#include <assert.h>
#include <stdio.h>
#include "../../../src/bsp/spi/bsp_spi.c"

int main(void) {
    assert(bsp_spi_init() == EXIT_OK);
    const uint8_t bytes[] = {0x2A, 0, 0, 0, 239};
    const uint16_t pixels[] = {0xF800, 0x07E0, 0x001F};
    assert(bsp_spi_cfg[BSP_SPI_DEV_LCD].inst == spi1);
    assert(mock_gpio_function[10] == GPIO_FUNC_SPI && mock_gpio_function[11] == GPIO_FUNC_SPI);
    assert(mock_gpio_function[16] == 0 && mock_gpio_function[17] == 0 && mock_gpio_function[18] == 0);
    mock_scheduler_state = taskSCHEDULER_NOT_STARTED;
    assert(bsp_spi_cs_assert(BSP_SPI_DEV_LCD, 10) == EXIT_NOT_INITIALIZED);
    mock_scheduler_state = taskSCHEDULER_RUNNING;
    assert(bsp_spi_set_baudrate(BSP_SPI_DEV_LCD, 1000000) == EXIT_OK);

    assert(bsp_spi_cs_assert(BSP_SPI_DEV_LCD, 10) == EXIT_OK);
    assert(bsp_spi_transfer(BSP_SPI_DEV_LCD, bytes, NULL, sizeof(bytes), 10) == EXIT_OK);
    assert(!spi_is_busy(spi1));  // DMA 中断已到、尾帧仍在移位的情形必须等待。
    assert(mock_gpio_level[9] == 0);
    assert(bsp_spi_transfer16(BSP_SPI_DEV_LCD, pixels, count_of(pixels), 10) == EXIT_OK);
    assert(!spi_is_busy(spi1));
    assert(bsp_spi_cs_release(BSP_SPI_DEV_LCD) == EXIT_OK && mock_gpio_level[9] == 1);

    assert(bsp_spi_cs_assert(BSP_SPI_DEV_LCD, 10) == EXIT_OK);
    mock_spi_stuck = true;
    const uint64_t before = mock_time_us;
    assert(bsp_spi_transfer16(BSP_SPI_DEV_LCD, pixels, count_of(pixels), 5) == EXIT_TIMEOUT);
    assert(mock_time_us - before < 6000 && mock_spi_reset_count == 1);
    assert(spi1->baudrate == 1000000);
    assert(bsp_spi_cs_release(BSP_SPI_DEV_LCD) == EXIT_OK);
    assert(bsp_spi_write(BSP_SPI_DEV_LCD, bytes, sizeof(bytes), 10) == EXIT_OK);

    mock_dma_complete = false;
    assert(bsp_spi_write(BSP_SPI_DEV_LCD, bytes, sizeof(bytes), 5) == EXIT_TIMEOUT);
    mock_dma_complete = true;
    assert(bsp_spi_write(BSP_SPI_DEV_LCD, bytes, sizeof(bytes), 10) == EXIT_OK);
    uint8_t rx[3];
    assert(bsp_spi_cs_assert(BSP_SPI_DEV_LCD, 10) == EXIT_OK);
    assert(bsp_spi_transfer(BSP_SPI_DEV_LCD, bytes, rx, sizeof(rx), 10) == EXIT_OK);
    assert(bsp_spi_transfer(BSP_SPI_DEV_LCD, bytes, NULL, sizeof(bytes), 10) == EXIT_OK);
    assert(bsp_spi_cs_release(BSP_SPI_DEV_LCD) == EXIT_OK);
    puts("PASS: SPI task context, pin allocation, tail completion and timeout recovery");
    return 0;
}
