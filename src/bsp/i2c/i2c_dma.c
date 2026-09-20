// 移植自 https://github.com/fivdi/pico-i2c-dma
// （MIT 许可证，Copyright (c) 2022 Brian Cooke），master @ 2022-09-03。
// 本地改动：设置 I2C 中断优先级（I2C_DMA_IRQ_PRIORITY）——原版未设置，
// NVIC 复位默认优先级 0 会违反本工程 RP2350 FreeRTOS 移植的 FromISR 约束。
#include "FreeRTOS.h"
#include "semphr.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "i2c_dma.h"

#define I2C_MAX_TRANSFER_SIZE     1056
// 1000ms 的传输超时足以让一次 10000 bit 的传输在低至 10000 波特的
// 波特率下也能顺利完成而不触发超时。
#define I2C_TRANSFER_TIMEOUT_MS   1000
#define I2C_TAKE_MUTEX_TIMEOUT_MS 10000
// 本地改动：I2C 中断优先级（数值越大优先级越低）。ISR 中调用了
// xSemaphoreGiveFromISR，优先级数值必须 >= configMAX_SYSCALL_INTERRUPT_PRIORITY
// （本工程 RP2350 移植用 BASEPRI 屏蔽，该值为 16），取最低档 0xF0 保证安全。
#define I2C_DMA_IRQ_PRIORITY      0xF0

typedef struct i2c_dma_s {
  i2c_inst_t *i2c;

  uint irq_num;
  irq_handler_t irq_handler;

  uint baudrate;
  uint sda_gpio;
  uint scl_gpio;

  SemaphoreHandle_t semaphore;
  SemaphoreHandle_t mutex;

  volatile bool stop_detected;
  volatile bool abort_detected;

  uint16_t data_cmds[I2C_MAX_TRANSFER_SIZE];
} i2c_dma_t;

static i2c_dma_t i2c_dma_list[2];

static void i2c_dma_irq_handler(i2c_dma_t *i2c_dma) {
  const uint32_t status = i2c_get_hw(i2c_dma->i2c)->intr_stat;

  // 如果发生中止（abort），通常先产生一次中止中断，随后再产生一次停止中断。
  // 但在极少数情况下，例如复位后的第一次 I2C 传输就被中止时，中止标志与
  // 停止标志看起来会在同一时刻或几乎同一时刻被置位。
  if (status & I2C_IC_INTR_STAT_R_TX_ABRT_BITS) {
    // 传输已被中止。
    i2c_get_hw(i2c_dma->i2c)->clr_tx_abrt;
    i2c_dma->abort_detected = true;
  }

  if (status & I2C_IC_INTR_STAT_R_STOP_DET_BITS) {
    // 传输完成。
    i2c_get_hw(i2c_dma->i2c)->clr_stop_det;
    i2c_dma->stop_detected = true;

    // 如果 xSemaphoreGiveFromISR 失败并返回 errQUEUE_FULL，这里不处理该错误，
    // 因为也做不了什么。一旦 xSemaphoreGiveFromISR 失败，对应的
    // xSemaphoreTake 调用最终会因超时而返回。
    BaseType_t task_switch_required = pdFALSE;
    xSemaphoreGiveFromISR(i2c_dma->semaphore, &task_switch_required);
    portYIELD_FROM_ISR(task_switch_required);
  }
}

static void i2c0_dma_irq_handler(void) {
  i2c_dma_irq_handler(&i2c_dma_list[0]);
}

static void i2c1_dma_irq_handler(void) {
  i2c_dma_irq_handler(&i2c_dma_list[1]);
}

static void i2c_dma_set_target_addr(i2c_inst_t *i2c, uint8_t addr) {
  i2c_get_hw(i2c)->enable = 0;
  i2c_get_hw(i2c)->tar = addr;
  i2c_get_hw(i2c)->enable = 1;
}

static void i2c_dma_tx_channel_configure(
  i2c_inst_t *i2c, int tx_channel, const uint16_t *tx_buf, size_t len
) {
  dma_channel_config tx_config = dma_channel_get_default_config(tx_channel);
  channel_config_set_read_increment(&tx_config, true);
  channel_config_set_write_increment(&tx_config, false);
  channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_16);
  channel_config_set_dreq(&tx_config, i2c_get_dreq(i2c, true));
  dma_channel_configure(
    tx_channel, &tx_config, &i2c_get_hw(i2c)->data_cmd, tx_buf, len, true
  );
}

static void i2c_dma_rx_channel_configure(
  i2c_inst_t *i2c, int rx_channel, uint8_t *rx_buf, size_t len
) {
  dma_channel_config rx_config = dma_channel_get_default_config(rx_channel);
  channel_config_set_read_increment(&rx_config, false);
  channel_config_set_write_increment(&rx_config, true);
  channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_8);
  channel_config_set_dreq(&rx_config, i2c_get_dreq(i2c, false));
  dma_channel_configure(
    rx_channel, &rx_config, rx_buf, &i2c_get_hw(i2c)->data_cmd, len, true
  );
}

static void i2c_dma_pin_open_drain(uint gpio) {
  gpio_set_function(gpio, GPIO_FUNC_SIO);
  gpio_set_dir(gpio, GPIO_IN);
  gpio_put(gpio, 0);
}

static void i2c_dma_pin_od_low(uint gpio) {
  gpio_set_dir(gpio, GPIO_OUT);
}

static void i2c_dma_pin_od_high(uint gpio) {
  gpio_set_dir(gpio, GPIO_IN);
}

static void i2c_dma_unblock(i2c_dma_t *i2c_dma) {
  i2c_dma_pin_open_drain(i2c_dma->sda_gpio);
  i2c_dma_pin_open_drain(i2c_dma->scl_gpio);

  bool sda_high;
  int max_tries = 9;

  // 确保软件模拟（bit-banging）的 I2C 时钟频率不超过 100KHz。
  const uint32_t f_clk_sys_khz =
    frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
  const uint32_t i2c_delay = f_clk_sys_khz / 100 / 2;

  do {
    i2c_dma_pin_od_low(i2c_dma->scl_gpio);
    for (int i = i2c_delay; i > 0; i -= 1) {
      __asm__("nop");
    }

    i2c_dma_pin_od_high(i2c_dma->scl_gpio);
    for (int i = i2c_delay; i > 0; i -= 1) {
      __asm__("nop");
    }

    max_tries -= 1;
    sda_high = gpio_get(i2c_dma->sda_gpio);
  } while (!sda_high && max_tries > 0);
}

static bool i2c_dma_is_blocked(i2c_dma_t *i2c_dma) {
  i2c_dma_pin_open_drain(i2c_dma->sda_gpio);
  i2c_dma_pin_open_drain(i2c_dma->scl_gpio);

  const bool sda_high = gpio_get(i2c_dma->sda_gpio);
  const bool scl_high = gpio_get(i2c_dma->scl_gpio);

  return !sda_high || !scl_high;
}

static int i2c_dma_init_intern(i2c_dma_t *i2c_dma) {
  irq_set_enabled(i2c_dma->irq_num, false);

  i2c_dma->stop_detected = false;
  i2c_dma->abort_detected = false;

  if (uxSemaphoreGetCount(i2c_dma->semaphore) != 0) {
    if (xSemaphoreTake(i2c_dma->semaphore, 0) != pdTRUE) {
      return PICO_ERROR_GENERIC;
    }
  }

  // 这里不要对 i2c_dma->mutex 做任何操作，交给 i2c_dma_write_read 处理。
  // 另外，用 xSemaphoreCreateMutex 创建的互斥量在创建后立刻就可以成功获取。

  // 尝试解除总线阻塞。如果无法解除，也继续往下执行。
  if (i2c_dma_is_blocked(i2c_dma)) {
    i2c_dma_unblock(i2c_dma);
  }

  i2c_init(i2c_dma->i2c, i2c_dma->baudrate);

  gpio_set_function(i2c_dma->sda_gpio, GPIO_FUNC_I2C);
  gpio_set_function(i2c_dma->scl_gpio, GPIO_FUNC_I2C);
  gpio_pull_up(i2c_dma->sda_gpio);
  gpio_pull_up(i2c_dma->scl_gpio);

  i2c_get_hw(i2c_dma->i2c)->intr_mask =
    I2C_IC_INTR_MASK_M_STOP_DET_BITS |
    I2C_IC_INTR_MASK_M_TX_ABRT_BITS;

  irq_set_exclusive_handler(i2c_dma->irq_num, i2c_dma->irq_handler);
  // 本地改动：显式设置中断优先级，详见文件开头说明。
  irq_set_priority(i2c_dma->irq_num, I2C_DMA_IRQ_PRIORITY);
  irq_set_enabled(i2c_dma->irq_num, true);

  return PICO_OK;
}

static int i2c_dma_reinit(i2c_dma_t *i2c_dma) {
  return i2c_dma_init_intern(i2c_dma);
}

int i2c_dma_init(
  i2c_dma_t **pi2c_dma,
  i2c_inst_t *i2c,
  uint baudrate,
  uint sda_gpio,
  uint scl_gpio
) {
  i2c_dma_t *i2c_dma;

  if (i2c == i2c0) {
    i2c_dma = &i2c_dma_list[0];
    i2c_dma->i2c = i2c0;
    i2c_dma->irq_num = I2C0_IRQ;
    i2c_dma->irq_handler = i2c0_dma_irq_handler;
  } else {
    i2c_dma = &i2c_dma_list[1];
    i2c_dma->i2c = i2c1;
    i2c_dma->irq_num = I2C1_IRQ;
    i2c_dma->irq_handler = i2c1_dma_irq_handler;
  }

  *pi2c_dma = i2c_dma;

  i2c_dma->baudrate = baudrate;
  i2c_dma->sda_gpio = sda_gpio;
  i2c_dma->scl_gpio = scl_gpio;

  i2c_dma->semaphore = xSemaphoreCreateBinary();
  if (i2c_dma->semaphore == NULL) {
    return PICO_ERROR_GENERIC;
  }

  i2c_dma->mutex = xSemaphoreCreateMutex();
  if (i2c_dma->mutex == NULL) {
    return PICO_ERROR_GENERIC;
  }

  return i2c_dma_init_intern(i2c_dma);
}

static int i2c_dma_write_read_internal(
  i2c_dma_t *i2c_dma,
  uint8_t addr,
  const uint8_t *wbuf,
  size_t wbuf_len,
  uint8_t *rbuf,
  size_t rbuf_len
) {
  if (
    (wbuf_len > 0 && wbuf == NULL) ||
    (rbuf_len > 0 && rbuf == NULL) ||
    (wbuf_len == 0 && rbuf_len == 0) ||
    (wbuf_len + rbuf_len > I2C_MAX_TRANSFER_SIZE)
  ) {
    return PICO_ERROR_INVALID_ARG;
  }

  const bool writing = (wbuf_len > 0);
  const bool reading = (rbuf_len > 0);

  int tx_chan = 0; // 用于把 data_cmds 写入 I2C 外设的通道。
  int rx_chan = 0; // 需要时用于从 I2C 外设读取数据的通道。

  if (writing) {
    // 为要写入 I2C 总线的每个字节设置命令。
    for (size_t i = 0; i != wbuf_len; ++i) {
      i2c_dma->data_cmds[i] = wbuf[i];
    }

    // 写入的第一个字节之前必须有一个起始条件。
    i2c_dma->data_cmds[0] |= I2C_IC_DATA_CMD_RESTART_BITS;
  }

  // 无论写还是读，都需要 DMA 的 tx_chan 通道。
  tx_chan = dma_claim_unused_channel(false);
  if (tx_chan == -1) {
    return PICO_ERROR_GENERIC;
  }

  if (reading) {
    // 为要从 I2C 总线读取的每个字节设置命令。
    for (size_t i = 0; i != rbuf_len; ++i) {
      i2c_dma->data_cmds[wbuf_len + i] = I2C_IC_DATA_CMD_CMD_BITS;
    }

    // 读取的第一个字节之前必须有一个起始/重复起始条件。
    i2c_dma->data_cmds[wbuf_len] |= I2C_IC_DATA_CMD_RESTART_BITS;

    // 只有读操作才需要 DMA 的 rx_chan 通道。
    rx_chan = dma_claim_unused_channel(false);
    if (rx_chan == -1) {
      dma_channel_unclaim(tx_chan);
      return PICO_ERROR_GENERIC;
    }
  }

  // 最后传输的那个字节之后必须跟一个停止条件。
  i2c_dma->data_cmds[wbuf_len + rbuf_len - 1] |= I2C_IC_DATA_CMD_STOP_BITS;

  // 告诉 I2C 外设本次传输的目标设备地址。
  i2c_dma_set_target_addr(i2c_dma->i2c, addr);

  i2c_dma->stop_detected = false;
  i2c_dma->abort_detected = false;

  // 在所需的 DMA 通道上启动 I2C 传输。
  if (reading) {
    i2c_dma_rx_channel_configure(i2c_dma->i2c, rx_chan, rbuf, rbuf_len);
  }
  i2c_dma_tx_channel_configure(
    i2c_dma->i2c, tx_chan, i2c_dma->data_cmds, wbuf_len + rbuf_len
  );

  // 通过 DMA 的 I2C 传输已经启动，现在等待它完成。正常情况下，当总线上检测到
  // 停止条件时传输即告完成。如果硬件在传输过程中检测到问题，通常会出现一次中止
  // 随后再跟一次停止。也存在检测不到停止和/或中止的情况，这时就需要超时机制来
  // 兜底。例如，当 SDA 被持续拉低时，就检测不到停止条件。
  const bool timeout = xSemaphoreTake(
    i2c_dma->semaphore, I2C_TRANSFER_TIMEOUT_MS * portTICK_PERIOD_MS
  ) == pdFALSE;

  // 如果出现问题，则中止 DMA。
  if (timeout || i2c_dma->abort_detected || !i2c_dma->stop_detected) {
    dma_channel_abort(tx_chan);
    if (reading) {
      dma_channel_abort(rx_chan);
    }
  }

  // 释放 DMA 通道。
  dma_channel_unclaim(tx_chan);
  if (reading) {
    dma_channel_unclaim(rx_chan);
  }

  int rc = PICO_OK;

  if (timeout) {
    rc = PICO_ERROR_TIMEOUT;
  } else if (i2c_dma->abort_detected || !i2c_dma->stop_detected) {
    rc = PICO_ERROR_IO;
  }

  // 尝试从错误中恢复。
  if (rc != PICO_OK) {
    i2c_dma_reinit(i2c_dma);
  }

  return rc;
}

int i2c_dma_write_read(
  i2c_dma_t *i2c_dma,
  uint8_t addr,
  const uint8_t *wbuf,
  size_t wbuf_len,
  uint8_t *rbuf,
  size_t rbuf_len
) {
  if (xSemaphoreTake(
      i2c_dma->mutex, I2C_TAKE_MUTEX_TIMEOUT_MS * portTICK_PERIOD_MS
    ) != pdTRUE) {
    return PICO_ERROR_TIMEOUT;
  }

  const int rc = i2c_dma_write_read_internal(
    i2c_dma, addr, wbuf, wbuf_len, rbuf, rbuf_len
  );

  if (xSemaphoreGive(i2c_dma->mutex) != pdTRUE && rc == PICO_OK) {
    return PICO_ERROR_GENERIC;
  }

  return rc;
}
