#pragma once

// BSP 层的 SPI 总线抽象。
//
// 设计要点（为什么是这套 API）：
//
// 1) 总线所有权就是 CS。bsp_spi_cs_assert() 在拉低 CS 的同时拿住这个 SPI 实例的
//    互斥量；bsp_spi_cs_release() 等波形真正发完再抬 CS、放锁。两者严格绑定，所以
//    "CS 低电平窗口"和"总线被独占的时间"始终一致，不会出现两个任务各发一包、
//    包间 CS 交错拉低这种只有半个事务被保护的情况。
//    pin_cs 配成 BSP_SPI_PIN_NONE（CS 硬件常低的设备）时，这两个函数退化为纯锁操作，
//    其余语义不变。
//
// 2) 只有一个传输函数。写 / 读 / 全双工不是三个 API，而是 bsp_spi_transfer() 里
//    tx、rx 是否为 NULL 的三种组合（见其注释）。少两条代码路径、少两组测试，
//    也不会再出现名字撒谎的情况——"write_begin / write_end"在读事务里就是错的。
//
// 3) 传输函数既不碰 CS 也不碰锁，只负责"把这一段字节搬上空口"。要切成多段（命令 /
//    参数 / 像素各一段）就在 assert 和 release 之间多调几次；中途可以随意切 DC 之类
//    的边带信号，因为 transfer 返回时这一段已经真正发完，而不是"塞进 FIFO 就返回"。
//
// 4) DC、复位脚这类边带信号不归这里管，由 driver 用 bsp_gpio 操作。"这一段是命令还是
//    参数"是协议语义，属于 driver 的知识；BSP 只认 CS 和字节流。

#include "hardware/spi.h"

#include "lib/tools/common_def.h"
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"

// 未使用的引脚占位：没有 MISO 的写-only 设备、CS 硬件常低的设备。
#define BSP_SPI_PIN_NONE         0xFFu

// 传给 timeout_ms 表示无限等待。
#define BSP_SPI_TIMEOUT_FOREVER  UINT32_MAX

typedef enum {
    BSP_SPI_DEV_LCD,
    BSP_SPI_DEV_COUNT,        // 仅用于边界检查，不是真实设备
} BSP_SPI_DEV;

// 静态配置表，在 .c 里按 BSP_SPI_DEV 下标逐项给出。
typedef struct {
    spi_inst_t *inst;                   // spi0 / spi1
    u32 pin_sck, pin_mosi, pin_miso;    // pin_miso 可为 BSP_SPI_PIN_NONE
    u32 pin_cs;                         // 可为 BSP_SPI_PIN_NONE
    bool cs_active_low;                 // 外部低有效时置 true
    u32 baudrate_hz;                    // 期望频率；外部器件能跑多快就配多快
    u8  cpol, cpha;                     // 0 / 1
    u8  dummy_tx;                       // 只读传输时 TX 通道反复发的字节
} bsp_spi_dev_cfg_t;

// 初始化静态表里的所有设备：配置引脚、申请常驻 DMA 通道、创建同步原语、
// 安装 DMA 完成中断。CS 会被置于无效电平。
// 必须在任何其他 bsp_spi_* 之前调用，且只能调一次。
exit_code_t bsp_spi_init(void);

// 改波特率。内部会短暂关闭外设重算分频。
// 不要在 cs_assert 和 cs_release 之间调用，否则会打断进行中的事务。
exit_code_t bsp_spi_set_baudrate(BSP_SPI_DEV dev, u32 baudrate_hz);

// ---- 事务边界：CS 即总线所有权 ----

// 开始一个事务：在 timeout_ms 内拿到该设备的总线互斥量，然后拉低 CS。
//
// 返回 EXIT_OK 表示总线已独占、CS 已有效，必须配对调用一次 cs_release()。
// 返回 EXIT_BUSY 表示超时没抢到总线——此时既没有拿锁也没有拉 CS，
// 所以**不要**在失败路径上调 cs_release()。
exit_code_t bsp_spi_cs_assert(BSP_SPI_DEV dev, u32 timeout_ms);

// 结束一个事务：等 BSY 清零（发送真正的最后一个比特），抬 CS，放锁。
//
// 为什么必须等 BSY：DMA 计数归零只代表字节都进了 FIFO，最后一个字节可能还在
// 移位寄存器里；此时抬 CS 会把它截断（现象是最后一个参数字节 / 像素丢失）。
// 这个等待有界——最多排空 FIFO 深度（8 字节）的移位时间，所以不需要超时参数。
//
// 幂等：没有对应的 assert 时返回 EXIT_FAIL 且什么都不做，绝不会去抬别的
// 调用方正在用的 CS。
//
// 所有失败路径都必须走到这里，包括 transfer 返回错误之后，否则 CS 永远拉低、
// 总线再也拿不回来。
exit_code_t bsp_spi_cs_release(BSP_SPI_DEV dev);

// ---- 单段传输 ----

// 在 cs_assert / cs_release 之间传一段字节。tx / rx 的组合决定方向：
//
//   tx != NULL, rx == NULL   只写：DMA 从 tx 搬到 SPI 发送口，收到的字节丢弃。
//   tx == NULL, rx != NULL   只读：TX 通道反复发同一个 dummy_tx 字节造时钟，
//                            RX 通道把收到的字节写进 rx。
//   tx != NULL, rx != NULL   全双工：两条 DMA 通道等长对发对收，以 RX 完成
//                            为准（它一定晚于 TX）。
//   tx == NULL, rx == NULL   EXIT_INVALID_PARAM。
//
// 必须在 cs_assert 之后、cs_release 之前调用。没有先 assert 就调，返回
// EXIT_NOT_INITIALIZED——传输函数自己不拿锁，也不管 CS。
//
// timeout_ms 只约束"本段 DMA 完成"，不包含抢总线的时间（锁在 assert 时已经拿到，
// 那一步有自己的超时）。这一点上语义比一体的 write() 更清楚：拿到 EXIT_TIMEOUT
// 一定是总线时序出了问题，而不是被别的任务挡了一下。
//
// len 为 0 返回 EXIT_INVALID_PARAM；len 上限为 2^32-1（单次 DMA 计数上限），
// 更大的流请分成多段。
//
// 返回时本段已真正发完（BSY 已清零），所以两次 transfer 之间可以安全地切 DC
// 或读写别的边带信号。
exit_code_t bsp_spi_transfer(BSP_SPI_DEV dev, const u8 *tx, u8 *rx,
                             size_t len, u32 timeout_ms);

// ============================ 典型用法 ============================
//
// 写一包"命令 + 参数"（ST7789，DC 由 driver 用 bsp_gpio 控制）：
//
//     exit_code_t lcd_cmd(u8 cmd, const u8 *params, size_t n, u32 t) {
//         exit_code_t rc = bsp_spi_cs_assert(BSP_SPI_DEV_LCD, t);
//         if (rc != EXIT_OK) return rc;
//
//         bsp_gpio_set_active(BSP_GPIO_LCD_DC, false);   // 命令
//         rc = bsp_spi_transfer(BSP_SPI_DEV_LCD, &cmd, NULL, 1, t);
//
//         if (rc == EXIT_OK && n > 0) {
//             bsp_gpio_set_active(BSP_GPIO_LCD_DC, true); // 参数
//             rc = bsp_spi_transfer(BSP_SPI_DEV_LCD, params, NULL, n, t);
//         }
//
//         const exit_code_t end_rc = bsp_spi_cs_release(BSP_SPI_DEV_LCD);
//         return (rc != EXIT_OK) ? rc : end_rc;
//     }
//
// 读回（4 线 SPI，命令段 DC=0、数据段 DC=1，同一个 CS 窗口内完成）：
//
//     bsp_spi_cs_assert(BSP_SPI_DEV_LCD, t);
//       bsp_gpio_set_active(BSP_GPIO_LCD_DC, false);
//       bsp_spi_transfer(BSP_SPI_DEV_LCD, &cmd, NULL, 1, t);
//       bsp_gpio_set_active(BSP_GPIO_LCD_DC, true);
//       bsp_spi_transfer(BSP_SPI_DEV_LCD, dummy, rx, 4, t);   // 前 1 字节是 dummy
//     bsp_spi_cs_release(BSP_SPI_DEV_LCD);
//
// 大块像素流：整包一个 CS 窗口，中间标好窗口寄存器就直接推像素缓冲。
//
// ============================ 约束与边界 ============================
//
// - 不可在中断上下文调用，这些函数会阻塞（等锁、等 DMA）。
// - 一个事务内可以调多次 transfer，但不能调用其他设备的任何 bsp_spi_* 函数。
// - 互斥量用 xSemaphoreCreateMutex 创建，非递归：同一个任务在事务中再次
//   cs_assert 同一个设备会卡到超时后返回 EXIT_BUSY。
// - 本层不保证 CS 窗口内的字节序、命令语义，也不做字节交换；16bpp 数据的
//   字节序由 driver 在写缓冲时解决，或后续在 DMA 上用硬件 bswap。
// - DMA 源缓冲在整个 transfer 期间必须有效。同步接口返回时传输已完成，
//   所以栈上的缓冲是安全的。
//
// ============================== 暂不提供 ==============================
//
// - 异步提交 / 完成回调 / 双缓冲：用于让"渲染下一帧"和"发送当前帧"重叠。
//   需要给人一个段队列，等有实际需求再定。
// - 重复填充（源地址 ring）：纯色 fill_rect 可以只给 16 字节 pattern 让 DMA 重发，
//   省掉铺满 RAM。属于优化项，不影响接口形状。
exit_code_t bsp_spi_write(BSP_SPI_DEV dev, const u8 *tx, size_t len, u32 timeout_ms);
exit_code_t bsp_spi_read(BSP_SPI_DEV dev, const u8 *rx, size_t len, u32 timeout_ms);