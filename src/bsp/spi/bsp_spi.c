#include "hardware/dma.h"
#include "hardware/irq.h"
#include "bsp_spi.h"
#include "FreeRTOS.h"
#include "semphr.h"

exit_code_t bsp_spi_init();
exit_code_t bsp_spi_set_baudrate(BSP_SPI_DEV dev, u32 baudrate_hz);

exit_code_t bsp_spi_write(BSP_SPI_DEV dev, const u8 *tx, size_t len, u32 timeout_ms);
exit_code_t bsp_spi_read(BSP_SPI_DEV dev, u8 *rx, size_t len, u32 timeout_ms);
exit_code_t bsp_spi_transfer(BSP_SPI_DEV dev, const u8 *tx, u8 *rx, size_t len, u32 timeout_ms);