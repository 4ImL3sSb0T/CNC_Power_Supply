#ifndef _I2C_DMA_EXIT_H_
#define _I2C_DMA_EXIT_H_

#include "pico/error.h"
#include "lib/tools/common_def.h"

// 把 pico-i2c-dma（driver/i2c/i2c_dma.h）的 PICO_* 返回码翻译成本项目的
// exit_code_t 约定，调用方不必直接依赖 PICO_* 常量。
static inline exit_code_t i2c_dma_exit_code(int rc) {
    switch (rc) {
        case PICO_OK:
            return EXIT_OK;
        case PICO_ERROR_TIMEOUT:
            return EXIT_TIMEOUT;
        case PICO_ERROR_INVALID_ARG:
            return EXIT_INVALID_PARAM;
        case PICO_ERROR_IO:
        case PICO_ERROR_GENERIC:
        default:
            return EXIT_FAIL;
    }
}

#endif
