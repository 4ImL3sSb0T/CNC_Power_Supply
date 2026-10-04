# 固件引脚分配

当前配置以 Pico 2（RP2350A）为目标。表中列出本工程已配置的外设引脚；SPI CS 使用软件 GPIO 控制。

| 外设 | 信号 | GPIO |
| --- | --- | --- |
| IMU / I2C0 | SDA | 16 |
| IMU / I2C0 | SCL | 17 |
| LCD / SPI1 | SCK | 10 |
| LCD / SPI1 | MOSI | 11 |
| LCD | CS（低有效） | 9 |
| LCD | DC | 8 |
| LCD | 背光（低有效） | 25 |
| 状态 LED | 输出 | 3 |
| 风扇 PWM | 输出 | 18 |
| 背光 PWM | 输出 | 20 |
| 电压设定 PWM | 输出 | 22 |
| 电流设定 PWM | 输出 | 23 |
| 蜂鸣器 PWM | 输出 | 24 |
| ADC | 输入电压 | 26 |
| ADC | 输出电压 | 27 |
| ADC | PG | 28 |

LCD 只发送数据，不配置 MISO。原 LCD 配置占用了 IMU 的 GPIO16/17，且 SCK 与风扇共用了 GPIO18，现移到 SPI1 的 GPIO9/10/11。若实物按原配置接线，需要将 LCD 的 CS、SCK、MOSI 分别改接 GPIO9、GPIO10、GPIO11；IMU 引脚保持不变。

LCD 初始化和刷新依赖 FreeRTOS 的 DMA 完成信号量，必须在调度器启动后的任务中调用。当前 `main.c` 创建一次性 LCD 测试任务负责初始化与首次刷新。

内部温度传感器使用 SDK 的 `ADC_TEMPERATURE_CHANNEL_NUM`，没有外部引脚。PG/温度单次转换后清空 FIFO，再从通道 0 恢复输入、输出电压轮询，保证 DMA 缓冲中两个通道的顺序一致。
