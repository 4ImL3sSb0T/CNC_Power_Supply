#include "imu.h"
#include "bsp/i2c/i2c_dma_exit.h"
#include "stdio.h"

#if IMU_USE_DMA
#include "bsp/i2c/i2c_dma.h"
#include "hardware/irq.h"
static i2c_dma_t *imu_i2c = NULL;
#endif

static i2c_inst_t *imu_i2c_instance = NULL;
static u32 imu_sdl_pin = 0, imu_scl_pin = 0;
static bool imu_initialized = false;

exit_code_t imu_init(i2c_inst_t *i2c_instance, u32 sdl_pin, u32 scl_pin) {
    if (imu_initialized) {
        return EXIT_ALREADY_INITIALIZED;
    }

#if IMU_USE_DMA
    // 初始化只做总线配置，不发起任何传输：此时调度器可能尚未启动，
    // i2c_dma 的等待依赖信号量阻塞，只能在任务上下文中使用。
    exit_code_t rc = i2c_dma_exit_code(
        i2c_dma_init(&imu_i2c, i2c_instance, IMU_I2C_BAUDRATE_HZ, sdl_pin, scl_pin));
    if (rc != EXIT_OK) {
        imu_i2c = NULL;
        return rc;
    }
#else
    // 阻塞读取对照模式：直接使用 SDK 的 i2c_*_blocking
    u32 act_baudrate = i2c_init(i2c_instance, IMU_I2C_BAUDRATE_HZ);
    printf("I2C initialized at %u Hz\n", act_baudrate);
    gpio_set_function(sdl_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sdl_pin);
    gpio_pull_up(scl_pin);
#endif

    imu_i2c_instance = i2c_instance;
    imu_sdl_pin = sdl_pin;
    imu_scl_pin = scl_pin;
    imu_initialized = true;

    return EXIT_OK;
}

/**
 * 从寄存器 reg 起连续读 len 字节（设备支持地址自增）。
 * DMA 模式：任务阻塞在信号量上等中断通知；阻塞模式：在 tight_loop 里空转等待。
 */
static exit_code_t imu_read_reg(u8 reg, u8 *buf, u32 len) {
#if IMU_USE_DMA
    return i2c_dma_exit_code(
        i2c_dma_write_read(imu_i2c, IMU_I2C_ADDRESS, &reg, 1, buf, len));
#else
    int ret = i2c_write_blocking(imu_i2c_instance, IMU_I2C_ADDRESS, &reg, 1, true);
    if (ret != 1) {
        return EXIT_FAIL;
    }
    ret = i2c_read_blocking(imu_i2c_instance, IMU_I2C_ADDRESS, buf, len, false);
    if (ret != (int)len) {
        return EXIT_FAIL;
    }
    return EXIT_OK;
#endif
}

/**
 * 读取版本寄存器（0x01 起连续 3 字节）并打印。
 * 只能在任务上下文中调用：传输完成前任务会阻塞等待。
 */
exit_code_t imu_probe_version(void) {
    if (!imu_initialized) {
        return EXIT_NOT_INITIALIZED;
    }

    u8 data[IMU_VERSION_LEN];
    exit_code_t rc = imu_read_reg(IMU_REG_VERSION, data, IMU_VERSION_LEN);
    if (rc != EXIT_OK) {
        printf("Failed to read IMU version, rc=%d\n", rc);
        return rc;
    }

    printf("Version of IMU: %02X %02X %02X\n", data[0], data[1], data[2]);
    return EXIT_OK;
}

exit_code_t imu_deinit(void) {
    if (!imu_initialized) {
        return EXIT_NOT_INITIALIZED;
    }

#if IMU_USE_DMA
    // 库没有提供 deconfig：先禁用 I2C 中断（handler 留在向量表中但不会再触发）。
    // 注意库不释放信号量，deinit 后再次 init 会重新创建、旧的泄漏。
    irq_set_enabled(imu_i2c_instance == i2c0 ? I2C0_IRQ : I2C1_IRQ, false);
    imu_i2c = NULL;
#endif

    i2c_deinit(imu_i2c_instance);
    gpio_set_function(imu_sdl_pin, GPIO_FUNC_NULL); // 恢复 GPIO 为默认状态
    gpio_set_function(imu_scl_pin, GPIO_FUNC_NULL);

    imu_i2c_instance = NULL;
    imu_initialized = false;
    return EXIT_OK;
}

exit_code_t imu_get_accel(vec3f *accel) {
    if (!imu_initialized) {
        return EXIT_NOT_INITIALIZED;
    }

    u8 data[IMU_ACCEL_LEN];
    // 一次事务完成“写寄存器号 + 重复起始 + 连续读 6 字节”
    exit_code_t rc = imu_read_reg(IMU_REG_ACCEL, data, IMU_ACCEL_LEN);
    if (rc != EXIT_OK) {
        return rc;
    }

    accel->x = (i16)((data[1] << 8) | data[0]) * IMU_ACCEL_SCALE;
    accel->y = (i16)((data[3] << 8) | data[2]) * IMU_ACCEL_SCALE;
    accel->z = (i16)((data[5] << 8) | data[4]) * IMU_ACCEL_SCALE;
    return EXIT_OK;
}
