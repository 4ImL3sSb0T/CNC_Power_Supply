#include "hardware/i2c.h"
#include "pico/stdlib.h"
#include "lib/tools/common_def.h"
#include "lib/tools/vec_math.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "driver/imu/imu_config.h"

exit_code_t imu_init(i2c_inst_t *i2c_instance, u32 sdl_pin, u32 scl_pin);
exit_code_t imu_deinit();
exit_code_t imu_probe_version(void);
exit_code_t imu_get_accel(vec3f *accel);
exit_code_t imu_get_gyro(vec3f *gyro);
exit_code_t imu_get_mag(vec3f *mag);