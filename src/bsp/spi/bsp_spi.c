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
#include "pico/time.h"

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
        .inst = spi1,
        // 保留 IMU 的 GPIO16/17 与风扇 PWM 的 GPIO18，LCD 使用 SPI1。
        // CS 仍由 SIO 软件控制；写入型面板不占用 MISO。实际接线见 docs/hardware.md。
        .pin_sck = 10, .pin_mosi = 11, .pin_miso = BSP_SPI_PIN_NONE,
        .pin_cs = 9,
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
    u8  bits;                   // 当前 DSS（8 / 16），切格式前必须等移位结束
    u32 baudrate_hz;             // 复位恢复时保留当前实际波特率
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

// DMA 完成仅代表数据入 FIFO，必须等 BSY 清零后才能改 DC、数据宽度或 CS。
static bool bsp_spi_wait_shift_done(spi_inst_t *inst, u64 deadline_us, bool forever) {
    while (spi_is_busy(inst)) {
        if (!forever && time_us_64() >= deadline_us) return false;
        tight_loop_contents();
    }
    return true;
}

// 超时后停止 DMA 并复位 SPI，丢弃未发送的帧和完成信号，允许后续事务重新开始。
static void bsp_spi_abort_and_reset(bsp_spi_rt_t *rt) {
    // 与另一核上的 ISR 串行清理，避免超时之后旧 ISR 才补发完成信号。
    taskENTER_CRITICAL();
    dma_irqn_set_channel_enabled(0, (uint)rt->tx_ch, false);
    dma_irqn_set_channel_enabled(0, (uint)rt->rx_ch, false);
    dma_channel_abort((uint)rt->tx_ch);
    dma_channel_abort((uint)rt->rx_ch);
    dma_irqn_acknowledge_channel(0, (uint)rt->tx_ch);
    dma_irqn_acknowledge_channel(0, (uint)rt->rx_ch);
    (void)xSemaphoreTake(rt->done, 0);
    taskEXIT_CRITICAL();
    spi_deinit(rt->cfg->inst);
    spi_init(rt->cfg->inst, rt->baudrate_hz);
    spi_set_format(rt->cfg->inst, 8,
                   rt->cfg->cpol ? SPI_CPOL_1 : SPI_CPOL_0,
                   rt->cfg->cpha ? SPI_CPHA_1 : SPI_CPHA_0, SPI_MSB_FIRST);
    rt->bits = 8;
}

static bool bsp_spi_use_bits(bsp_spi_rt_t *rt, u8 bits, u64 deadline_us, bool forever) {
    if (rt->bits == bits) return true;
    if (!bsp_spi_wait_shift_done(rt->cfg->inst, deadline_us, forever)) return false;
    spi_set_format(rt->cfg->inst, bits,
                   rt->cfg->cpol ? SPI_CPOL_1 : SPI_CPOL_0,
                   rt->cfg->cpha ? SPI_CPHA_1 : SPI_CPHA_0, SPI_MSB_FIRST);
    rt->bits = bits;
    return true;
}

// DMA 等待和尾帧排空共用同一截止时间，不给每个阶段重新计时。
static exit_code_t bsp_spi_wait_segment(bsp_spi_rt_t *rt, u64 deadline_us, bool forever) {
    TickType_t ticks = portMAX_DELAY;
    if (!forever) {
        const u64 now_us = time_us_64();
        const u64 remaining_us = deadline_us > now_us ? deadline_us - now_us : 0;
        ticks = pdMS_TO_TICKS((remaining_us + 999u) / 1000u);
    }
    if (xSemaphoreTake(rt->done, ticks) == pdTRUE &&
        bsp_spi_wait_shift_done(rt->cfg->inst, deadline_us, forever)) {
        return EXIT_OK;
    }
    bsp_spi_abort_and_reset(rt);
    return EXIT_TIMEOUT;
}

// 每次只开"算完成"的那条通道的中断，ISR 因此不必判断这次是谁完成。
//
// 但两条通道的原始完成位（INTR）都要先清掉，INTE 只控制是否触发中断。
// 上一次全双工传输里被关掉中断的 TX 通道，完成时照样会把 INTS 置上而没人应答；
// 等到下一次只写传输打开 TX 中断时，这个陈旧的置位会立刻触发一次中断，让
// 等待中的任务以为传输已经完成——接着抬 CS、截断整帧。
static void bsp_spi_irq_select(bsp_spi_rt_t *rt, int done_ch) {
    if (rt->tx_ch >= 0) {
        dma_irqn_acknowledge_channel(0, (uint)rt->tx_ch);
        dma_irqn_set_channel_enabled(0, (uint)rt->tx_ch, done_ch == rt->tx_ch);
    }
    if (rt->rx_ch >= 0) {
        dma_irqn_acknowledge_channel(0, (uint)rt->rx_ch);
        dma_irqn_set_channel_enabled(0, (uint)rt->rx_ch, done_ch == rt->rx_ch);
    }
}

static void bsp_spi_dma_irq_handler(void) {
    const UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
    BaseType_t woken = pdFALSE;
    for (u32 i = 0; i < count_of(bsp_spi_rt); ++i) {
        bsp_spi_rt_t *rt = &bsp_spi_rt[i];
        if (!rt->inited) continue;

        int fired = -1;
        if (rt->tx_ch >= 0 && dma_irqn_get_channel_status(0, (uint)rt->tx_ch)) {
            fired = rt->tx_ch;
        }
        if (rt->rx_ch >= 0 && dma_irqn_get_channel_status(0, (uint)rt->rx_ch)) {
            fired = rt->rx_ch;
        }
        if (fired < 0) continue;

        // 先应答再给信号量：反过来的话，应答前如果这次传输的完成位又被置上，
        // 会被随后的应答一起清掉，那次完成就没有中断了。
        dma_irqn_acknowledge_channel(0, (uint)fired);

        xSemaphoreGiveFromISR(rt->done, &woken);
    }
    taskEXIT_CRITICAL_FROM_ISR(saved);
    portYIELD_FROM_ISR(woken);
}

exit_code_t bsp_spi_init(void) {
    if (bsp_spi_inited) return EXIT_ALREADY_INITIALIZED;

    for (u32 i = 0; i < count_of(bsp_spi_cfg); i++) {
        const bsp_spi_dev_cfg_t *cfg = &bsp_spi_cfg[i];
        bsp_spi_rt_t *rt = &bsp_spi_rt[i];

        rt->cfg = cfg;
        rt->tx_ch = -1;
        rt->rx_ch = -1;
        rt->dummy = cfg->dummy_tx;
        rt->bits = 8;

        rt->mutex = xSemaphoreCreateMutex();
        rt->done = xSemaphoreCreateBinary();
        if (rt->mutex == NULL || rt->done == NULL) return EXIT_NO_MEMORY;

        rt->tx_ch = dma_claim_unused_channel(false);
        rt->rx_ch = dma_claim_unused_channel(false);
        if (rt->tx_ch < 0 || rt->rx_ch < 0) {
            // 把已经拿到的那条还回去，避免半初始化状态吃掉一条通道
            if (rt->tx_ch >= 0) dma_channel_unclaim((uint)rt->tx_ch);
            if (rt->rx_ch >= 0) dma_channel_unclaim((uint)rt->rx_ch);
            rt->tx_ch = -1;
            rt->rx_ch = -1;
            return EXIT_NO_RESOURCE;
        }

        rt->baudrate_hz = spi_init(cfg->inst, cfg->baudrate_hz);
        spi_set_format(cfg->inst, 8, cfg->cpol ? SPI_CPOL_1 : SPI_CPOL_0, cfg->cpha ? SPI_CPHA_1 : SPI_CPHA_0, SPI_MSB_FIRST);

        gpio_set_function(cfg->pin_sck, GPIO_FUNC_SPI);
        gpio_set_function(cfg->pin_mosi, GPIO_FUNC_SPI);

        if (cfg->pin_miso != BSP_SPI_PIN_NONE) gpio_set_function(cfg->pin_miso, GPIO_FUNC_SPI);

        if (cfg->pin_cs != BSP_SPI_PIN_NONE) {
            gpio_init(cfg->pin_cs);                 // 置为 GPIO_FUNC_SIO
            gpio_set_dir(cfg->pin_cs, GPIO_OUT);
            bsp_spi_cs(rt, false);
        }

        rt->inited = true;
    }

    irq_set_exclusive_handler(DMA_IRQ_0, bsp_spi_dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, BSP_SPI_IRQ_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);

    bsp_spi_inited = true;
    return EXIT_OK;
}

exit_code_t bsp_spi_set_baudrate(BSP_SPI_DEV dev, u32 baudrate_hz) {
    if (!bsp_spi_valid(dev)) return EXIT_INVALID_PARAM;
    if (!bsp_spi_inited) return EXIT_NOT_INITIALIZED;

    bsp_spi_rt_t *rt = &bsp_spi_rt[dev];

    if (xSemaphoreTake(rt->mutex, pdMS_TO_TICKS(BSP_SPI_LOCK_TIMEOUT_MS)) != pdTRUE) return EXIT_BUSY;

    rt->baudrate_hz = spi_set_baudrate(rt->cfg->inst, baudrate_hz);

    xSemaphoreGive(rt->mutex);
    return EXIT_OK;
}

exit_code_t bsp_spi_cs_assert(BSP_SPI_DEV dev, u32 timeout_ms) {
    if (!bsp_spi_valid(dev)) return EXIT_INVALID_PARAM;
    if (!bsp_spi_inited || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return EXIT_NOT_INITIALIZED;

    bsp_spi_rt_t *rt = &bsp_spi_rt[dev];
    spi_inst_t *inst = rt->cfg->inst;

    // pdMS_TO_TICKS(0) 就是 0 节拍，即"只试一次不等待"
    const TickType_t ticks = (timeout_ms == BSP_SPI_TIMEOUT_FOREVER)
                           ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(rt->mutex, ticks) != pdTRUE) return EXIT_BUSY;

    // 上一次超时的传输可能留下了完成信号（ISR 在我们超时之后才给出），
    // 不清掉的话这次的 transfer 会"秒完成"。
    if (uxSemaphoreGetCount(rt->done) != 0) (void)xSemaphoreTake(rt->done, 0);

    // TX-only 传输期间 RX FIFO 必然溢出（PL022 只置标志、继续移位），
    // 把残留数据和 sticky overrun 标志清掉，避免污染下一次读。
    while (spi_is_readable(inst)) (void)spi_get_hw(inst)->dr;
    spi_get_hw(inst)->icr = SPI_SSPICR_RORIC_BITS;

    bsp_spi_cs(rt, true);
    return EXIT_OK;
}

exit_code_t bsp_spi_cs_release(BSP_SPI_DEV dev) {
    if (!bsp_spi_valid(dev)) return EXIT_INVALID_PARAM;
    if (!bsp_spi_inited) return EXIT_NOT_INITIALIZED;

    bsp_spi_rt_t *rt = &bsp_spi_rt[dev];

    // 幂等：不是本任务持有的事务就什么都不做——绝不去抬别人正在用的 CS。
    // 同时也覆盖了"assert 没拿到锁就失败"的那种失败路径。
    if (!bsp_spi_owns(rt)) return EXIT_FAIL;

    // 正常 transfer 已等到移位结束；异常情况下收尾也必须有界并归还互斥量。
    const u64 deadline_us = time_us_64() + (u64)BSP_SPI_LOCK_TIMEOUT_MS * 1000u;
    const bool finished = bsp_spi_wait_shift_done(rt->cfg->inst, deadline_us, false);
    if (!finished) bsp_spi_abort_and_reset(rt);
    bsp_spi_cs(rt, false);
    xSemaphoreGive(rt->mutex);
    return finished ? EXIT_OK : EXIT_TIMEOUT;
}

exit_code_t bsp_spi_transfer(BSP_SPI_DEV dev, const u8 *tx, u8 *rx, size_t len, u32 timeout_ms) {
    if (!bsp_spi_valid(dev)) return EXIT_INVALID_PARAM;
    if (len == 0 || (tx == NULL && rx == NULL)) return EXIT_INVALID_PARAM;
    if (!bsp_spi_inited) return EXIT_NOT_INITIALIZED;

    bsp_spi_rt_t *rt = &bsp_spi_rt[dev];
    if (!bsp_spi_owns(rt)) return EXIT_NOT_INITIALIZED;   // 必须先 cs_assert

    spi_inst_t *inst = rt->cfg->inst;

    const bool forever = timeout_ms == BSP_SPI_TIMEOUT_FOREVER;
    const u64 deadline_us = forever ? 0 : time_us_64() + (u64)timeout_ms * 1000u;
    if (!bsp_spi_use_bits(rt, 8, deadline_us, forever)) {
        bsp_spi_abort_and_reset(rt);
        return EXIT_TIMEOUT;
    }
    // 同一 CS 窗口内前面的只写段也会留下 RX 数据，避免后续读段收到旧帧。
    while (spi_is_readable(inst)) (void)spi_get_hw(inst)->dr;
    spi_get_hw(inst)->icr = SPI_SSPICR_RORIC_BITS;

    dma_channel_config tx_cfg = dma_channel_get_default_config(rt->tx_ch);
    channel_config_set_transfer_data_size(&tx_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&tx_cfg, tx != NULL);
    channel_config_set_write_increment(&tx_cfg, false);
    channel_config_set_dreq(&tx_cfg, spi_get_dreq(inst, true));
    channel_config_set_high_priority(&tx_cfg, true);

    if (rx != NULL) {
        dma_channel_config rx_cfg = dma_channel_get_default_config((uint)rt->rx_ch);
        channel_config_set_transfer_data_size(&rx_cfg, DMA_SIZE_8);
        channel_config_set_read_increment(&rx_cfg, false);   // 源固定 = SPI 数据口
        channel_config_set_write_increment(&rx_cfg, true);   // 目的递增 = 调用方缓冲
        channel_config_set_dreq(&rx_cfg, spi_get_dreq(inst, false));
        channel_config_set_high_priority(&rx_cfg, true);

        // 先武装 RX 再一起启动。两条通道必须在同一次寄存器写里触发：
        // 分两次 dma_channel_start 的话，RX 还没武装好时先到的字节会顶掉数据。
        dma_channel_configure((uint)rt->rx_ch, &rx_cfg, rx,
                              &spi_get_hw(inst)->dr, len, false);
        dma_channel_configure((uint)rt->tx_ch, &tx_cfg, &spi_get_hw(inst)->dr,
                              tx != NULL ? tx : &rt->dummy, len, false);

        bsp_spi_irq_select(rt, rt->rx_ch);   // 等 RX：它一定晚于 TX 完成
        dma_start_channel_mask((1u << rt->rx_ch) | (1u << rt->tx_ch));
    } else {
        dma_channel_configure((uint)rt->tx_ch, &tx_cfg, &spi_get_hw(inst)->dr,
                              tx, len, false);

        bsp_spi_irq_select(rt, rt->tx_ch);
        dma_channel_start((uint)rt->tx_ch);
    }

    return bsp_spi_wait_segment(rt, deadline_us, forever);
}

exit_code_t bsp_spi_transfer16(BSP_SPI_DEV dev, const u16 *tx, size_t count, u32 timeout_ms) {
    if (!bsp_spi_valid(dev)) return EXIT_INVALID_PARAM;
    if (tx == NULL || count == 0) return EXIT_INVALID_PARAM;
    if (!bsp_spi_inited) return EXIT_NOT_INITIALIZED;

    bsp_spi_rt_t *rt = &bsp_spi_rt[dev];
    if (!bsp_spi_owns(rt)) return EXIT_NOT_INITIALIZED;   // 必须先 cs_assert

    spi_inst_t *inst = rt->cfg->inst;

    // 16 位下 PL022 自己按 MSB 先出一个半字，调用方的 u16 缓冲按正常内存顺序给即可。
    const bool forever = timeout_ms == BSP_SPI_TIMEOUT_FOREVER;
    const u64 deadline_us = forever ? 0 : time_us_64() + (u64)timeout_ms * 1000u;
    if (!bsp_spi_use_bits(rt, 16, deadline_us, forever)) {
        bsp_spi_abort_and_reset(rt);
        return EXIT_TIMEOUT;
    }

    dma_channel_config tx_cfg = dma_channel_get_default_config((uint)rt->tx_ch);
    channel_config_set_transfer_data_size(&tx_cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&tx_cfg, true);
    channel_config_set_write_increment(&tx_cfg, false);
    channel_config_set_dreq(&tx_cfg, spi_get_dreq(inst, true));
    channel_config_set_high_priority(&tx_cfg, true);

    dma_channel_configure((uint)rt->tx_ch, &tx_cfg, &spi_get_hw(inst)->dr, tx, count, false);

    bsp_spi_irq_select(rt, rt->tx_ch);
    dma_channel_start((uint)rt->tx_ch);

    return bsp_spi_wait_segment(rt, deadline_us, forever);
}

// 一次调用 = 一个完整事务（assert + transfer + release）。给不需要分段
// 边带信号的短包用；要在一个 CS 窗口里切 DC、或拼多段数据就自己写三段式。
exit_code_t bsp_spi_write(BSP_SPI_DEV dev, const u8 *tx, size_t len, u32 timeout_ms) {
     exit_code_t rc = bsp_spi_cs_assert(dev, timeout_ms);
    if (rc != EXIT_OK) return rc;

    rc = bsp_spi_transfer(dev, tx, NULL, len, timeout_ms);

    const exit_code_t end_rc = bsp_spi_cs_release(dev);
    return (rc != EXIT_OK) ? rc : end_rc;
}

exit_code_t bsp_spi_read(BSP_SPI_DEV dev, const u8 *rx, size_t len, u32 timeout_ms) {
    exit_code_t rc = bsp_spi_cs_assert(dev, timeout_ms);
    if (rc != EXIT_OK) return rc;

    // rx 是输出缓冲，这里的 const 是被头文件签名带上的（那两行应该改成 u8 *rx）。
    // transfer 需要可写指针，先强转掉；改签名后这行直接去掉即可。
    rc = bsp_spi_transfer(dev, NULL, (u8 *)rx, len, timeout_ms);

    const exit_code_t end_rc = bsp_spi_cs_release(dev);
    return (rc != EXIT_OK) ? rc : end_rc;
}
