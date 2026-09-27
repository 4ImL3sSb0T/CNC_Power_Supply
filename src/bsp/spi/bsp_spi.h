#include "hardware/spi.h"

#include "lib/tools/common_def.h"

typedef enum {
    BSP_SPI_DEV_LCD,
} BSP_SPI_DEV;

typedef struct {
    spi_inst_t *inst;
    u32 pin_sck, pin_mosi, pin_miso;
    u32 pin_cs;
    bool cs_active_low;
    u32 baudrate_hz;
    u8  cpol, cpha;
} bsp_spi_dev_cfg_t;

exit_code_t bsp_spi_init();
exit_code_t bsp_spi_set_baudrate(BSP_SPI_DEV dev, u32 baudrate_hz);

exit_code_t bsp_spi_write(BSP_SPI_DEV dev, const u8 *tx, size_t len, u32 timeout_ms);
exit_code_t bsp_spi_read(BSP_SPI_DEV dev, u8 *rx, size_t len, u32 timeout_ms);
exit_code_t bsp_spi_transfer(BSP_SPI_DEV dev, const u8 *tx, u8 *rx, size_t len, u32 timeout_ms);