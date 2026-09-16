#include "imu.h"
#include "stdio.h"

#define IMU_I2C_ADDRESS 0x23

static i2c_inst_t *imu_i2c_instance = NULL;
static u32 imu_sdl_pin = 0, imu_scl_pin = 0;

/**
 * 从 I2C 从设备的指定寄存器读取数据
 * @param i2c     I2C 实例 (i2c0 或 i2c1)
 * @param addr    从设备 7 位地址（不含读写位）
 * @param reg     要读取的寄存器地址
 * @param len     要读取的字节数
 * @param buf     存放读取数据的缓冲区
 * @return 0 成功，非 0 失败
 */
static int i2c_read_register_blocking(uint8_t reg, uint8_t len, uint8_t *buf) {
    // 第 1 步：发送寄存器地址，nostop = true 保持总线控制
    if (imu_i2c_instance == NULL) {
        return 1; // I2C 实例未初始化
    }

    int ret = i2c_write_blocking(imu_i2c_instance, IMU_I2C_ADDRESS, &reg, 1, true);
    if (ret != 1) {          // 期望写入 1 字节，不等于 1 说明失败
        return 1;
    }

    // 第 2 步：从同一从设备读取 len 字节，nostop = false 结束时发 STOP
    ret = i2c_read_blocking(imu_i2c_instance, IMU_I2C_ADDRESS, buf, len, false);
    if (ret != (int)len) {   // 期望读取 len 字节，不等于说明失败
        return 1;
    }

    return 0;
}

exit_code_t imu_init(i2c_inst_t *i2c_instance, u32 sdl_pin, u32 scl_pin) {
    u32 act_baudrate = i2c_init(i2c_instance, 100 * 1000); // Initialize I2C at 100kHz
    imu_i2c_instance = i2c_instance;
    printf("I2C initialized at %u Hz\n", act_baudrate);
    gpio_set_function(sdl_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sdl_pin);
    gpio_pull_up(scl_pin);

    imu_sdl_pin = sdl_pin;
    imu_scl_pin = scl_pin;

    u8 data[3];
    i2c_read_register_blocking(0x01, 3, data); // Read 3 bytes from register 0x00

    printf("Version of IMU: %02X %02X %02X\n", data[0], data[1], data[2]);

    return EXIT_OK;
}

exit_code_t imu_deinit() {
    i2c_deinit(imu_i2c_instance); // Deinitialize I2C0
    gpio_set_function(imu_sdl_pin, GPIO_FUNC_NULL); // Reset GPIO 21 to default state
    gpio_set_function(imu_scl_pin, GPIO_FUNC_NULL); // Reset GPIO 22 to default state

    imu_i2c_instance = NULL;
}

exit_code_t imu_get_accel(vec3f *accel) {
    u8 data[6];
    i2c_read_register_blocking(0x04, 6, data);
    float ratio = 16.0f / 32768.0f; 
    accel->x = (int16_t)((data[1] << 8) | data[0]) * ratio;
    accel->y = (int16_t)((data[3] << 8) | data[2]) * ratio;
    accel->z = (int16_t)((data[5] << 8) | data[4]) * ratio;
    return EXIT_OK;
}
exit_code_t imu_get_gyro(vec3f *gyro);
exit_code_t imu_get_mag(vec3f *mag);
