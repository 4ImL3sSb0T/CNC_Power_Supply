# CNC_Power_Supply — 项目备忘（给 AI / 协作者）

## 0. 最高优先级：了解代码先走 codegraph

本仓库已建好 **CodeGraph** 索引（CLI `codegraph` v1.6.0，数据在 `.codegraph/`）。
凡是「这段功能在哪 / 谁调用了它 / 改这里会牵连什么 / 给我某个模块的上下文」这类问题，
**先用 codegraph 定位，再决定要不要打开文件**；不要一上来就全仓库 grep 或逐个 read。

索引范围 = 本工程自己的代码：`main.c` + `src/{app,bsp,driver,lib}`，共 **25 个文件 / 349 个符号**。
`src/third_party/`（FreeRTOS-Kernel、FreeRTOS-Kernel-Community-Supported-Ports、CMSIS-DSP）
是 git 子模块、几千个文件，已通过根目录 `codegraph.json` 的 `exclude` 排除，
否则检索结果会被第三方代码淹没。需要看它们时直接读文件 / 查官方文档。

### 常用命令（在仓库根目录执行）

| 目的 | 命令 |
| --- | --- |
| 按名字找符号 | `codegraph query <name>` |
| 一片区域的源码 + 调用链，一把梭 | `codegraph explore "<关键词...>" --max-files 4` |
| 为某个任务攒上下文 | `codegraph context "<任务描述>"` |
| 单个符号的源码 + 调用者/被调用者 | `codegraph node <symbol>` |
| 谁调用了它 | `codegraph callers <symbol>` |
| 它调用了谁 | `codegraph callees <symbol>` |
| 改动的爆炸半径 | `codegraph impact <symbol>` |
| 改动波及哪些测试 | `codegraph affected <files...>` |
| 文件结构 | `codegraph files` |
| 索引状态 | `codegraph status` |
| 增量更新索引 | `codegraph sync` |
| 全量重建 | `codegraph index` |

### 使用要点

- `explore` / `node` 输出的是**磁盘上逐字节一致、带行号的源码**，等于已经读过这些文件，
  不要再用 read 重复读；它没覆盖到的地方才另开一次 explore 或 read。
- 改完 C 代码后跑一次 `codegraph sync`（毫秒级，随时可跑）；只有索引损坏或大改目录结构才
  `codegraph index` 全量重建。索引带 MCP daemon 自动同步。
- 报 `database is locked` 是 daemon 正在写库，**等一两秒重试 `codegraph sync`** 即可，
  不要去杀 daemon。
- `.codegraph/` 是本地缓存（已 gitignore）；`codegraph.json` 是配置，要提交。

## 1. 工程速览

- 目标：**RP2350（Pico 2）+ FreeRTOS 双核 + CMSIS-DSP** 的 CNC 电源固件。
- 构建：CMake + Pico SDK 2.3.1（`PICO_BOARD=pico2`）+ Ninja，`build/` 由 Pico VS Code
  扩展生成（`cmake --build build` / `ninja -C build` 均可）。
- 分层：`main.c`（建任务 + CPU 占用率统计）→ `src/driver/*`（imu、led）→
  `src/bsp/*`（adc、gpio、i2c、spi、pwm）→ `src/lib/*`（fifo_buffer、vec_math、common_def）。
- `src/app/FreeRTOSConfig.h` 是**唯一的 FreeRTOS 配置点**；内核/移植/Heap4 由根
  `CMakeLists.txt` 链接。
- IMU 读取有 DMA（`src/bsp/i2c/i2c_dma.c`，移植自 pico-i2c-dma）与 SDK 阻塞两种模式，
  由 `src/driver/imu/imu_config.h` 的 `IMU_USE_DMA` 切换，用于对照 CPU 占用率。
- ISR 优先级约束：RP2350 移植用 BASEPRI 屏蔽，`configMAX_SYSCALL_INTERRUPT_PRIORITY = 16`；
  在 ISR 里调 `*FromISR` 的中断优先级数值必须 >= 16（`i2c_dma.c` 取 0xF0）。
- 文档：`docs/hardware.md`、`docs/pin_map.md`。

## 2. 约定

- 注释、文档用**中文**；提交信息与变量名保持既有风格（C11，`u8/u16/u32/i16` 等别名见
  `src/lib/tools/common_def.h`，返回码统一用 `exit_code_t`）。
- 改动尽量小：不要顺手重构无关代码，不要引入新依赖。
- 改完 C 代码至少跑一次构建验证，并说明验证结果（命令 + 输出摘要）。
