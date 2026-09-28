// BSP 层 SPI 总线实现（DMA 传输 + CS 即总线所有权）。
//
// 数据通路：SPI0/SPI1 的 TX/RX FIFO 各挂一条常驻 DMA 通道，通道由 DREQ 驱动，
// 所以一段传输启动之后 CPU 不参与搬运，只在末尾等一次完成信号。
//
// 同步模型：每个设备一把互斥量。bsp_spi_cs_assert() 拿锁 + 拉低 CS，
// bsp_spi_cs_release() 等 BSY 清零 + 抬 CS + 放锁——"总线被独占的时间"和
// "CS 低电平窗口"严格是同一个区间。bsp_spi_transfer() 自己不碰锁也不碰 CS，
// 只负责把一段字节搬上空口，因此必须在 assert 与 release 之间调用。
//
// CS 之所以用 SIO 软件控制而不是 PL022 的硬件 CSn（F1 功能）：硬件 CSn 在
// TX FIFO 排空时就抬起，DMA 分块喂 FIFO 的间隙会让它中途抖动，从机可能把
// 一包命令拆成两包。所以本文件里 CS 引脚一直保持 GPIO_FUNC_SIO。
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "bsp_spi.h"

// 抢总线 / 等外设的缺省上限。cs_assert、set_baudrate 用它；
// transfer 的超时由调用方按"这段数据在线上要跑多久"自己给。
#define BSP_SPI_LOCK_TIMEOUT_MS   1000u

// 数值越大优先级越低。ISR 里要调 xSemaphoreGiveFromISR，优先级数值必须
// >= configMAX_SYSCALL_INTERRUPT_PRIORITY（本工程 RP2350 移植为 16），
// 取最低档 0xF0 保证安全。同 src/bsp/i2c/i2c_dma.c 的处理。
#define BSP_SPI_IRQ_PRIORITY      0xF0u

static const bsp_spi_dev_cfg_t bsp_spi_cfg[] = {
    [BSP_SPI_DEV_LCD] = {
        .inst = spi0,
        // GPIO16~19 同一组 SPI0 功能引脚；CS 走 SIO 软件控制，
        // 不能改成 GPIO_FUNC_SPI，否则会切到硬件 CSn 上（见文件头说明）。
        .pin_sck = 18, .pin_mosi = 19, .pin_miso = 16,
        .pin_cs = 17,
        .cs_active_low = true,
        .baudrate_hz = 40u * 1000u * 1000u,
        .cpol = 0, .cpha = 0,
        .dummy_tx = 0xFF,
    },
};

typedef struct {
    const bsp_spi_dev_cfg_t *cfg;
    int tx_ch;                  // 常驻 DMA 通道：热路径上不再做 claim 的全局扫描
    int rx_ch;
    u8  dummy;                  // cfg->dummy_tx 的运行时副本，放 SRAM 里让 TX 通道反复读
    SemaphoreHandle_t mutex;    // 总线所有权：assert 拿到，release 放掉
    SemaphoreHandle_t done;     // 本段 DMA 完成信号，由 ISR 给出
    volatile bool inited;
} bsp_spi_rt_t;

static bsp_spi_rt_t bsp_spi_rt[count_of(bsp_spi_cfg)];
static bool bsp_spi_inited = false;

static inline bool bsp_spi_valid(BSP_SPI_DEV dev) {
    return (u32)dev < count_of(bsp_spi_rt);
}

// pin_cs 为 BSP_SPI_PIN_NONE 的设备（CS 硬件常低）只剩锁语义，不动引脚。
static inline void bsp_spi_cs(const bsp_spi_rt_t *rt, bool assert) {
    if (rt->cfg->pin_cs == BSP_SPI_PIN_NONE) return;
    gpio_put(rt->cfg->pin_cs, rt->cfg->cs_active_low ? !assert : assert);
}

// 只有握着这把互斥量的任务才有权动这个设备的 CS 和 DMA 通道。
// 用它来判"有没有先 cs_assert"，同时挡住另一个任务绕过事务直接调 transfer
// 来抢走别人正在用的 DMA 通道。
static inline bool bsp_spi_owns(const bsp_spi_rt_t *rt) {
    return xSemaphoreGetMutexHolder(rt->mutex) == xTaskGetCurrentTaskHandle();
}

// 每次只开"算完成"的那条通道的中断，ISR 因此不必判断这次是谁完成。
//
// 但两条通道的状态位都要先清掉：INTS 是原始状态位，和 INTE 使能位无关。
// 上一次全双工传输里被关掉中断的 TX 通道，完成时照样会把 INTS 置上而没人应答；
// 等到下一次只写传输打开 TX 中断时，这个陈旧的置位会立刻触发一次中断，让
// 等待中的任务以为传输已经完成——接着抬 CS、截断整帧。
static void bsp_spi_irq_select(bsp_spi_rt_t *rt, int done_ch) {

}

static void bsp_spi_dma_irq_handler(void) {

}

exit_code_t bsp_spi_init(void) {

}

exit_code_t bsp_spi_set_baudrate(BSP_SPI_DEV dev, u32 baudrate_hz) {

}

exit_code_t bsp_spi_cs_assert(BSP_SPI_DEV dev, u32 timeout_ms) {

}

exit_code_t bsp_spi_cs_release(BSP_SPI_DEV dev) {

}

exit_code_t bsp_spi_transfer(BSP_SPI_DEV dev, const u8 *tx, u8 *rx,
                             size_t len, u32 timeout_ms) {

}

// 一次调用 = 一个完整事务（assert + transfer + release）。给不需要分段
// 边带信号的短包用；要在一个 CS 窗口里切 DC、或拼多段数据就自己写三段式。
exit_code_t bsp_spi_write(BSP_SPI_DEV dev, const u8 *tx, size_t len, u32 timeout_ms) {

}

exit_code_t bsp_spi_read(BSP_SPI_DEV dev, const u8 *rx, size_t len, u32 timeout_ms) {

}
