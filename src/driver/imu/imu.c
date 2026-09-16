#include "imu.h"
#include "driver/i2c/i2c_dma.h"
#include "driver/i2c/i2c_dma_exit.h"
#include "hardware/irq.h"
#include "stdio.h"

static i2c_dma_t *imu_i2c = NULL;
static i2c_inst_t *imu_i2c_instance = NULL;
static u32 imu_sdl_pin = 0, imu_scl_pin = 0;

exit_code_t imu_init(i2c_inst_t *i2c_instance, u32 sdl_pin, u32 scl_pin) {
    if (imu_i2c != NULL) {
        return EXIT_ALREADY_INITIALIZED;
    }

    // 初始化只做总线配置，不发起任何传输：此时调度器可能尚未启动，
    // i2c_dma 的等待依赖信号量阻塞，只能在任务上下文中使用。
    exit_code_t rc = i2c_dma_exit_code(
        i2c_dma_init(&imu_i2c, i2c_instance, IMU_I2C_BAUDRATE_HZ, sdl_pin, scl_pin));
    if (rc != EXIT_OK) {
        imu_i2c = NULL;
        return rc;
    }

    imu_i2c_instance = i2c_instance;
    imu_sdl_pin = sdl_pin;
    imu_scl_pin = scl_pin;

    return EXIT_OK;
}

/**
 * 读取版本寄存器（0x01 起连续 3 字节）并打印。
 * 只能在任务上下文中调用：传输完成前任务会阻塞等待中断通知。
 */
exit_code_t imu_probe_version(void) {
    if (imu_i2c == NULL) {
        return EXIT_NOT_INITIALIZED;
    }

    u8 reg = IMU_REG_VERSION;
    u8 data[IMU_VERSION_LEN];
    exit_code_t rc = i2c_dma_exit_code(
        i2c_dma_write_read(imu_i2c, IMU_I2C_ADDRESS, &reg, 1, data, IMU_VERSION_LEN));
    if (rc != EXIT_OK) {
        printf("Failed to read IMU version, rc=%d\n", rc);
        return rc;
    }

    printf("Version of IMU: %02X %02X %02X\n", data[0], data[1], data[2]);
    return EXIT_OK;
}

exit_code_t imu_deinit(void) {
    if (imu_i2c == NULL) {
        return EXIT_NOT_INITIALIZED;
    }

    // 库没有提供 deconfig：先禁用 I2C 中断（handler 留在向量表中但不会再触发）。
    // 注意库不释放信号量，deinit 后再次 init 会重新创建、旧的泄漏。
    irq_set_enabled(imu_i2c_instance == i2c0 ? I2C0_IRQ : I2C1_IRQ, false);
    i2c_deinit(imu_i2c_instance);
    gpio_set_function(imu_sdl_pin, GPIO_FUNC_NULL); // 恢复 GPIO 为默认状态
    gpio_set_function(imu_scl_pin, GPIO_FUNC_NULL);

    imu_i2c = NULL;
    imu_i2c_instance = NULL;
    return EXIT_OK;
}

exit_code_t imu_get_accel(vec3f *accel) {
    if (imu_i2c == NULL) {
        return EXIT_NOT_INITIALIZED;
    }

    u8 reg = IMU_REG_ACCEL;
    u8 data[IMU_ACCEL_LEN];
    // 一次事务完成“写寄存器号 + 重复起始 + 连续读 6 字节”
    exit_code_t rc = i2c_dma_exit_code(
        i2c_dma_write_read(imu_i2c, IMU_I2C_ADDRESS, &reg, 1, data, IMU_ACCEL_LEN));
    if (rc != EXIT_OK) {
        return rc;
    }

    accel->x = (i16)((data[1] << 8) | data[0]) * IMU_ACCEL_SCALE;
    accel->y = (i16)((data[3] << 8) | data[2]) * IMU_ACCEL_SCALE;
    accel->z = (i16)((data[5] << 8) | data[4]) * IMU_ACCEL_SCALE;
    return EXIT_OK;
}
