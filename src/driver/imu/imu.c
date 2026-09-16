#include "imu.h"

static i2c_inst_t *imu_i2c_instance = NULL;
static u32 imu_sdl_pin = 0, imu_scl_pin = 0;

exit_code_t imu_init(i2c_inst_t *i2c_instance, u32 sdl_pin, u32 scl_pin) {
    u32 act_baudrate = i2c_init(i2c_instance, 100 * 1000); // Initialize I2C at 100kHz
    imu_i2c_instance = i2c_instance;
    printf("I2C initialized at %u Hz\n", act_baudrate);
    gpio_set_function(sdl_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(sdl_pin);
    gpio_pull_up(scl_pin);
}

exit_code_t imu_deinit() {
    i2c_deinit(imu_i2c_instance); // Deinitialize I2C0
    gpio_set_function(imu_sdl_pin, GPIO_FUNC_NULL); // Reset GPIO 21 to default state
    gpio_set_function(imu_scl_pin, GPIO_FUNC_NULL); // Reset GPIO 22 to default state

    imu_i2c_instance = NULL;
}

exit_code_t imu_get_accel(vec3f *accel);
exit_code_t imu_get_gyro(vec3f *gyro);
exit_code_t imu_get_mag(vec3f *mag);