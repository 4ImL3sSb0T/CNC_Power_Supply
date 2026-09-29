# 硬件引脚定义

本文档汇总「数控电源 v1 验证板」测试台固件（分支 `test/verify_pcb`）用到的全部 GPIO 分配。
**这里是索引，不是事实来源**：引脚编号的真实定义在各模块头文件的宏里（见每行的「定义位置」），
本表只做汇总。改引脚时先改代码，再回来同步本表。

- 适用固件：`main.c`（测试台入口），目标板 `PICO_BOARD=pico2`
- 最后核对：2026-09-28

---

## 1. 硬件组成

| 部件 | 说明 |
| --- | --- |
| MCU 板 | 正点原子 ATK-DNRP2350AM（RP2350A，Pico 2 目标） |
| 被测板 | 数控电源 v1 / SC8701 验证板，经 **H1 6P 排针**与 MCU 板相连 |
| 显示 | ST7789 1.14" 240×135，SPI1 + DMA |
| 按键 | KEY0 单键（MultiButton 状态机） |
| 上位机链路 | USB CDC 虚拟串口（`app/psu_link`），D+/D- 为芯片固定 USB pad，无 GPIO 编号 |

MCU 板与验证板是两块独立板卡：表中「板载外设」随 MCU 板固定，「测试台」部分走 H1 排针接到 SC8701。

---

## 2. 引脚分配总表

### 2.1 MCU 板载外设

| 功能 | GPIO | 复用 / 方向 | 关键参数 | 宏 | 定义位置 |
| --- | --- | --- | --- | --- | --- |
| 心跳 LED | GP3 | GPIO 输出 | 500ms 翻转 | `LED_GPIO_PIN` | `src/driver/led/led.h:29` |
| 按键 KEY0 | GP2 | GPIO 输入，内部上拉 | 低有效，勿开内部下拉（RP2350-E9） | `KEY0_GPIO_PIN` | `src/bsp/input/key.h:23` |
| LCD 背光 BL | GP25 | GPIO 输出 | 高=点亮 | `LCD_PIN_BL` | `src/bsp/lcd/lcd_priv.h:12` |
| LCD 命令/数据 DC | GP8 | GPIO 输出 | 0=命令，1=数据 | `LCD_PIN_DC` | `src/bsp/lcd/lcd_priv.h:13` |
| LCD 片选 CS | GP9 | GPIO 输出 | 低有效 | `LCD_PIN_CS` | `src/bsp/lcd/lcd_priv.h:14` |
| LCD SPI SCK | GP10 | SPI1 SCK | 62.5 MHz，CPOL=0/CPHA=0，MSB | `SPI_CLK_GPIO_PIN` | `src/bsp/lcd/spi.h:31` |
| LCD SPI MOSI | GP11 | SPI1 MOSI | 同上 | `SPI_MOSI_GPIO_PIN` | `src/bsp/lcd/spi.h:32` |
| LCD SPI MISO | GP12 | SPI1 MISO | 仅保留（ST7789 不回读） | `SPI_MISO_GPIO_PIN` | `src/bsp/lcd/spi.h:33` |
| IMU SDA | GP16 | I2C0 SDA | 100 kHz | `IMU_SDA_GPIO` | `src/config/board_config.h:33` |
| IMU SCL | GP17 | I2C0 SCL | 100 kHz | `IMU_SCL_GPIO` | `src/config/board_config.h:34` |
| 串口 stdio TX | GP0 | UART0 TX | SDK 默认值，非本工程显式定义 | — | Pico SDK `boards/pico2.h` |
| 串口 stdio RX | GP1 | UART0 RX | 同上 | — | Pico SDK `boards/pico2.h` |

> SPI 端口宏 `SPI_PORT = spi1`（`src/bsp/lcd/spi.h:30`），波特率 `SPI_BAUD_RATE = 62500000`。
> IMU 从地址 `0x23`、总线速率 `IMU_I2C_BAUDRATE_HZ = 100000`（`src/config/imu_config.h:9-10`）。
> `pico_enable_stdio_uart/usb(1)` 在 `CMakeLists.txt` 打开，日志同时走 UART0 与 USB CDC。

### 2.2 测试台（H1 排针 ↔ SC8701）

| 功能 | GPIO | 复用 | H1 脚位 | 关键参数 | 宏 | 定义位置 |
| --- | --- | --- | --- | --- | --- | --- |
| PWM 电压设定 | GP4 | PWM2 **A** | H1.3 | 50 kHz | `PCB_PIN_PWM` | `src/config/board_config.h:24` |
| IPWM 电流限 | GP5 | PWM2 **B** | H1.2 | 50 kHz，**严禁悬空** | `PCB_PIN_IPWM` | `src/config/board_config.h:25` |
| CE# 使能 | GP6 | GPIO 输出 | H1.5 | 低=使能；**悬空即使能** | `PCB_PIN_CE_N` | `src/config/board_config.h:26` |
| PG 电源良好节点 | GP26 | ADC0 (ch0) | H1.4 | 模拟读取，8 次平均 | `PCB_ADC_PG_GPIO` / `PCB_ADC_PG_CH` | `src/config/board_config.h:27-28` |
| VOUT 实测（可选） | GP27 | ADC1 (ch1) | 外接分压 | 默认关闭 | `PCB_ADC_VOUT_GPIO` / `PCB_ADC_VOUT_CH` | `src/config/board_config.h:29-30` |

两路 PWM 同属 **PWM slice 2**（A/B 通道），同频同 wrap（`src/bsp/power/pcb_ctrl.c:69-73`）。

### 2.3 H1 6P 排针网表

嘉立创 EDA「数控电源 v1 / 验证板」回读网表（2026-09-23）：

| H1 脚 | 信号 | 去向 |
| --- | --- | --- |
| 1 | AGND | 模拟地 |
| 2 | IPWM | MCU GP5（PWM2B） |
| 3 | PWM | MCU GP4（PWM2A） |
| 4 | PG 节点 | MCU GP26（ADC0） |
| 5 | CE# | MCU GP6 |
| 6 | 3V3 | 3.3V 供电 |

---

## 3. 未占用引脚

以下 GPIO 在当前固件中**未被引用**，可分配给新外设（分配后请更新本表）：

`GP7`、`GP13`–`GP15`、`GP18`–`GP24`、`GP28`、`GP29`

- `GP0`/`GP1` 名义上被 UART0 stdio 占用（SDK 默认），H1 未接线，需用时可关掉 UART stdio。
- `src/pio/blink.pio` 是示例程序，当前**没有任何 C 代码调用** `blink_program_init`，不占引脚。

---

## 4. 注意事项

1. **上电安全态**：SC8701 的 `/CE` 内部 1M 下拉，悬空即上电使能；`IPWM` 悬空芯片不能正常工作。
   所以 `main()` 第一件事是 `pcb_ctrl_init()`（`main.c:95`），把 CE# 关断、PWM/IPWM 先以 GPIO 拉低再切 PWM 复用
   （`src/bsp/power/pcb_ctrl.c:42-52`）。
2. **GP2 / ADC 数字输入**：RP2350-E9 影响内部下拉与数字输入缓冲，按键注释明确要求开内部上拉、勿开下拉；
   `adc_gpio_init()` 会关掉数字输入缓冲（`src/bsp/power/pcb_ctrl.c:79-80`）。
3. **PG 节点是模拟量**：故障时被 LED3 钳到 ≈2.1V，落在 MCU 数字阈值灰区，必须用 ADC 读并用滞回判定
   （阈值见 `board_config.h:54-59`）。
4. **VOUT 实测需外接分压**：VOUT → 9.1K → GP27 → 1K → AGND，接线后把 `PCB_TEST_VOUT_SENSE_ENABLE` 置 1。
5. **GP25 与板载 LED**：部分 RP2350 板把 GP25 接用户 LED，本工程将其用作 LCD 背光，勿复用。

---

## 5. 引脚变更检查清单

改任何引脚时，按顺序处理，避免漏改：

- [ ] 改对应宏（板载外设见各自 BSP 头文件，测试台引脚统一在 `src/config/board_config.h`）
- [ ] 确认新 GPIO 未与上表冲突，且外设功能（SPI1/I2C0/PWM2/ADC）支持该引脚
- [ ] 更新本文件第 2、3 节
- [ ] 若涉及 H1 接线，同步更新验证板原理图 / H1 网表（第 2.3 节）
- [ ] 涉及安全脚（CE#、IPWM）时，复查 `pcb_ctrl_init()` 的上电时序
