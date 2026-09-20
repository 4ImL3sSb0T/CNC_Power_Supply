#ifndef _I2C_DMA_H
#define _I2C_DMA_H

#include "hardware/i2c.h"

// 下面函数文档中所用符号的说明。
// --------------+------------------------------------------------------------
// S             | 起始条件。
// Sr            | 重复起始条件，用于从写模式切换到读模式。
// P             | 停止条件。
// Rd/Wr (1 bit) | 读/写位。Rd 为 1，Wr 为 0。
// A, NA (1 bit) | 应答（ACK）与非应答（NACK）位。
// addr (7 bits) | 7 位 I2C 地址。
// reg (8 bits)  | 寄存器字节，即通常用于选中器件上某个寄存器的数据字节。
// [..]          | 由 I2C 从器件发送的数据（相对于主机适配器发送的数据）。
// --------------+------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// i2c_dma_t 保存了 i2c_dma_* 系列函数驱动挂在 I2C 外设 I2C0 和 I2C1 上的
// I2C 器件所需的全部数据。调用 i2c_dma_init 可获取 I2C0 或 I2C1 对应的
// i2c_dma_t 指针。
typedef struct i2c_dma_s i2c_dma_t;

// 初始化 I2C 外设及其 SDA 引脚、SCL 引脚和波特率，使能外设，并为其使用
// DMA 做好准备。必须先调用 i2c_dma_init，再调用其他函数。该函数把指向
// i2c_dma_t 的指针拷贝到 *pi2c_dma。这个 i2c_dma_t 指针就是传给所有其他
// i2c_dma_* 函数第一个参数的指针。
//
// 返回值
//   PICO_OK
//     函数执行成功
//   PICO_ERROR_GENERIC
//     创建信号量失败
//     创建互斥量失败
//     获取信号量失败
int i2c_dma_init(
  i2c_dma_t **pi2c_dma, // 指向 i2c_dma_t 指针的指针
  i2c_inst_t *i2c,      // i2c0 或 i2c1
  uint baudrate,        // 波特率，单位赫兹
  uint sda_gpio,        // SDA 的 GPIO 编号
  uint scl_gpio         // SCL 的 GPIO 编号
);

// 在单次 I2C 传输中写入一段字节和/或读取一段字节。
//
// I2C 传输时序：
//
// 写入一段字节：
// S addr Wr [A] wbuf(0) [A] wbuf(1) [A] ... [A] wbuf(wbuf_len-1) [A] P
//
// 读取一段字节：
// S addr Rd [A] [rbuf(0)] A [rbuf(1)] A ... A [rbuf(rbuf_len-1)] NA P
//
// 写入一段字节再读取一段字节：
// S addr Wr [A] wbuf(0) [A] wbuf(1) [A] ... [A] wbuf(wbuf_len-1) [A]
//   Sr addr Rd [A] [rbuf(0)] A [rbuf(1)] A ... A [rbuf(rbuf_len-1)] NA P
//
// 返回值
//   PICO_OK
//     函数执行成功
//   PICO_ERROR_INVALID_ARG
//     传给函数的参数无效
//   PICO_ERROR_TIMEOUT
//     等待获取互斥量超时
//     等待 I2C 传输完成超时
//   PICO_ERROR_IO
//     I2C 传输被 I2C 外设中止
//     I2C 外设未检测到传输的停止条件
//   PICO_ERROR_GENERIC
//     释放互斥量失败
//     申请 DMA 通道失败
int i2c_dma_write_read(
  i2c_dma_t *i2c_dma,  // I2C0 或 I2C1 对应的 i2c_dma_t 指针
  uint8_t addr,        // 7 位 I2C 地址
  const uint8_t *wbuf, // 指向要写入的字节块的指针，或 NULL
  size_t wbuf_len,     // 要写入的字节块长度，或 0
  uint8_t *rbuf,       // 指向存放读取数据的字节块的指针，或 NULL
  size_t rbuf_len      // 要读取的数据字节数，或 0
);

// 写入一段字节。
//
// I2C 传输时序：
// S addr Wr [A] wbuf(0) [A] wbuf(1) [A] ... [A] wbuf(wbuf_len-1) [A] P
//
// 返回值
//   PICO_OK
//     函数执行成功
//   PICO_ERROR_INVALID_ARG
//     传给函数的参数无效
//   PICO_ERROR_TIMEOUT
//     等待获取互斥量超时
//     等待 I2C 传输完成超时
//   PICO_ERROR_IO
//     I2C 传输被 I2C 外设中止
//     I2C 外设未检测到传输的停止条件
//   PICO_ERROR_GENERIC
//     释放互斥量失败
//     申请 DMA 通道失败
static inline int i2c_dma_write(
  i2c_dma_t *i2c_dma,  // I2C0 或 I2C1 对应的 i2c_dma_t 指针
  uint8_t addr,        // 7 位 I2C 地址
  const uint8_t *wbuf, // 指向要写入的字节块的指针，或 NULL
  size_t wbuf_len      // 要写入的字节块长度，或 0
) {
  return i2c_dma_write_read(i2c_dma, addr, wbuf, wbuf_len, NULL, 0);
}

// 读取一段字节。
//
// I2C 传输时序：
// S addr Rd [A] [rbuf(0)] A [rbuf(1)] A ... A [rbuf(rbuf_len-1)] NA P
//
// 返回值
//   PICO_OK
//     函数执行成功
//   PICO_ERROR_INVALID_ARG
//     传给函数的参数无效
//   PICO_ERROR_TIMEOUT
//     等待获取互斥量超时
//     等待 I2C 传输完成超时
//   PICO_ERROR_IO
//     I2C 传输被 I2C 外设中止
//     I2C 外设未检测到传输的停止条件
//   PICO_ERROR_GENERIC
//     释放互斥量失败
//     申请 DMA 通道失败
static inline int i2c_dma_read(
  i2c_dma_t *i2c_dma, // I2C0 或 I2C1 对应的 i2c_dma_t 指针
  uint8_t addr,       // 7 位 I2C 地址
  uint8_t *rbuf,      // 指向存放读取数据的字节块的指针，或 NULL
  size_t rbuf_len     // 要读取的数据字节数，或 0
) {
  return i2c_dma_write_read(i2c_dma, addr, NULL, 0, rbuf, rbuf_len);
}

// 向某个寄存器写入一个字节。
// 
// I2C 传输时序：
// S addr Wr [A] reg [A] byte [A] P
//
// 返回值
//   PICO_OK
//     函数执行成功
//   PICO_ERROR_INVALID_ARG
//     传给函数的参数无效
//   PICO_ERROR_TIMEOUT
//     等待获取互斥量超时
//     等待 I2C 传输完成超时
//   PICO_ERROR_IO
//     I2C 传输被 I2C 外设中止
//     I2C 外设未检测到传输的停止条件
//   PICO_ERROR_GENERIC
//     释放互斥量失败
//     申请 DMA 通道失败
static inline int i2c_dma_write_byte(
  i2c_dma_t *i2c_dma, // I2C0 或 I2C1 对应的 i2c_dma_t 指针
  uint8_t addr,       // 7 位 I2C 地址
  uint8_t reg,        // 要写入的寄存器编号
  uint8_t byte        // 要写入的字节
) {
  const uint8_t wbuf[2] = {reg, byte};
  return i2c_dma_write_read(i2c_dma, addr, wbuf, 2, NULL, 0);
}

// 从某个寄存器读取一个字节。
// 
// I2C 传输时序：
// S addr Wr [A] reg [A] Sr addr Rd [A] [byte] NA P
//
// 返回值
//   PICO_OK
//     函数执行成功
//   PICO_ERROR_INVALID_ARG
//     传给函数的参数无效
//   PICO_ERROR_TIMEOUT
//     等待获取互斥量超时
//     等待 I2C 传输完成超时
//   PICO_ERROR_IO
//     I2C 传输被 I2C 外设中止
//     I2C 外设未检测到传输的停止条件
//   PICO_ERROR_GENERIC
//     释放互斥量失败
//     申请 DMA 通道失败
static inline int i2c_dma_read_byte(
  i2c_dma_t *i2c_dma, // I2C0 或 I2C1 对应的 i2c_dma_t 指针
  uint8_t addr,       // 7 位 I2C 地址
  uint8_t reg,        // 要读取的寄存器编号
  uint8_t *byte       // 指向存放读取数据的字节的指针
) {
  return i2c_dma_write_read(i2c_dma, addr, &reg, 1, byte, 1);
}

// 向某个寄存器写入一个 16 位字。先发送最低有效字节。
//
// I2C 传输时序：
// S addr Wr [A] reg [A] word lsb [A] word msb [A] P
//
// 返回值
//   PICO_OK
//     函数执行成功
//   PICO_ERROR_INVALID_ARG
//     传给函数的参数无效
//   PICO_ERROR_TIMEOUT
//     等待获取互斥量超时
//     等待 I2C 传输完成超时
//   PICO_ERROR_IO
//     I2C 传输被 I2C 外设中止
//     I2C 外设未检测到传输的停止条件
//   PICO_ERROR_GENERIC
//     释放互斥量失败
//     申请 DMA 通道失败
static inline int i2c_dma_write_word(
  i2c_dma_t *i2c_dma, // I2C0 或 I2C1 对应的 i2c_dma_t 指针
  uint8_t addr,       // 7 位 I2C 地址
  uint8_t reg,        // 要写入的寄存器编号
  uint16_t word       // 要写入的 16 位字
) {
  const uint8_t wbuf[3] = {reg, word & 0xff, word >> 8};
  return i2c_dma_write_read(i2c_dma, addr, wbuf, 3, NULL, 0);
}

// 从某个寄存器读取一个 16 位字。先接收最低有效字节。
//
// I2C 传输时序：
// S addr Wr [A] reg [A] Sr addr Rd [A] [word lsb] A [word msb] NA P
//
// 返回值
//   PICO_OK
//     函数执行成功
//   PICO_ERROR_INVALID_ARG
//     传给函数的参数无效
//   PICO_ERROR_TIMEOUT
//     等待获取互斥量超时
//     等待 I2C 传输完成超时
//   PICO_ERROR_IO
//     I2C 传输被 I2C 外设中止
//     I2C 外设未检测到传输的停止条件
//   PICO_ERROR_GENERIC
//     释放互斥量失败
//     申请 DMA 通道失败
static inline int i2c_dma_read_word(
  i2c_dma_t *i2c_dma, // I2C0 或 I2C1 对应的 i2c_dma_t 指针
  uint8_t addr,       // 7 位 I2C 地址
  uint8_t reg,        // 要读取的寄存器编号
  uint16_t *word      // 指向存放读取数据的 16 位字的指针
) {
  return i2c_dma_write_read(i2c_dma, addr, &reg, 1, (uint8_t *) word, 2);
}

// 向某个寄存器写入一个 16 位字。先发送最高有效字节。
//
// I2C 传输时序：
// S addr Wr [A] reg [A] word msb [A] word lsb [A] P
//
// 返回值
//   PICO_OK
//     函数执行成功
//   PICO_ERROR_INVALID_ARG
//     传给函数的参数无效
//   PICO_ERROR_TIMEOUT
//     等待获取互斥量超时
//     等待 I2C 传输完成超时
//   PICO_ERROR_IO
//     I2C 传输被 I2C 外设中止
//     I2C 外设未检测到传输的停止条件
//   PICO_ERROR_GENERIC
//     释放互斥量失败
//     申请 DMA 通道失败
static inline int i2c_dma_write_word_swapped(
  i2c_dma_t *i2c_dma, // I2C0 或 I2C1 对应的 i2c_dma_t 指针
  uint8_t addr,       // 7 位 I2C 地址
  uint8_t reg,        // 要写入的寄存器编号
  uint16_t word       // 要写入的 16 位字
) {
  const uint8_t wbuf[3] = {reg, word >> 8, word & 0xff};
  return i2c_dma_write_read(i2c_dma, addr, wbuf, 3, NULL, 0);
}

// 从某个寄存器读取一个 16 位字。先接收最高有效字节。
//
// I2C 传输时序：
// S addr Wr [A] reg [A] Sr addr Rd [A] [word msb] A [word lsb] NA P
//
// 返回值
//   PICO_OK
//     函数执行成功
//   PICO_ERROR_INVALID_ARG
//     传给函数的参数无效
//   PICO_ERROR_TIMEOUT
//     等待获取互斥量超时
//     等待 I2C 传输完成超时
//   PICO_ERROR_IO
//     I2C 传输被 I2C 外设中止
//     I2C 外设未检测到传输的停止条件
//   PICO_ERROR_GENERIC
//     释放互斥量失败
//     申请 DMA 通道失败
static inline int i2c_dma_read_word_swapped(
  i2c_dma_t *i2c_dma, // I2C0 或 I2C1 对应的 i2c_dma_t 指针
  uint8_t addr,       // 7 位 I2C 地址
  uint8_t reg,        // 要读取的寄存器编号
  uint16_t *word      // 指向存放读取数据的 16 位字的指针
) {
  int rc = i2c_dma_write_read(i2c_dma, addr, &reg, 1, (uint8_t *) word, 2);
  *word = *word << 8 | *word >> 8;
  return rc;
}

#ifdef __cplusplus
}
#endif

#endif
