#ifndef _IMU_CONFIG_H_
#define _IMU_CONFIG_H_

// 读取方式：1 = DMA（driver/i2c/i2c_dma）；0 = SDK 阻塞读取（i2c_*_blocking）。
// 两种模式都只影响读取路径本身，供 CPU 占用率对照测试切换。
#define IMU_USE_DMA           1

// IMU（I2C 从设备）地址与总线速率
#define IMU_I2C_ADDRESS       0x23
#define IMU_I2C_BAUDRATE_HZ   100000

// 寄存器地址（设备支持地址自增，多字节字段一次读出）
#define IMU_REG_VERSION       0x01  // 版本号，3 字节
#define IMU_REG_ACCEL         0x04  // 加速度 X/Y/Z 各 2 字节（小端）

// 字段长度（字节）
#define IMU_VERSION_LEN       3
#define IMU_ACCEL_LEN         6

// ±16g 量程下 ADC 读数 → g 的换算系数
#define IMU_ACCEL_SCALE       (16.0f / 32768.0f)

#endif
