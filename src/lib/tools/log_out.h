#ifndef _LOG_OUT_H_
#define _LOG_OUT_H_

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file        log_out.h
 * @brief       设备日志输出：统一行前缀 + 环形缓冲（等主机连上再一起吐）
 *
 * 行前缀：USB CDC 上协议帧与 printf 文本共用一条链路（见 app/psu_link/psu_link.h）。
 * 上位机靠 COBS 的 0x00 分块，把解不出合法帧的块当文本收下 —— 块里既有真日志行，
 * 也有被日志打断的半截帧。给每行加固定前缀后，上位机就能把两者精确分开。
 *
 * 环形缓冲：设备一重启，USB 就要重新枚举，主机把 DTR 拉起来之前写进 CDC 的字节
 * 全部被丢掉 —— 偏偏"为什么重启的"那几条启动日志最重要，等主机连上早就打完了。
 * 所以日志先进环形缓冲，由 log_drain() 在主机连上之后吐出去。缓冲满时丢最旧的，
 * 保住最近发生的事。
 *
 * 代价：日志不再从 printf 直接出去，屏幕/串口助手上看到的不再是实时的，而是
 * log_drain() 的节奏（链路任务每 5ms 一轮）。没接主机时（DTR 未拉起）连 UART
 * 上也不会有输出 —— stdio 的 UART 与 USB 是同一个出口，分不开。
 */

/* 与上位机 scripts/src/psu_host/protocol.py 的 LOG_LINE_PREFIX 必须一致 */
#define LOG_LINE_PREFIX "# "

/* 单条日志（含前缀与换行）的长度上限，超出的部分被截断。最长的一条是
 * app/syscfg 的就绪信息，约 90 字节，留一倍余量 */
#define LOG_LINE_MAX 160

/* 环形缓冲容量。启动那几条日志加起来不到 1KB，留 2KB 富余 */
#define LOG_BUF_SIZE 2048

/**
 * 记一行日志：加前缀、补换行、写进环形缓冲。**不直接输出**，由 log_drain() 负责吐出去。
 *
 * @param fmt printf 风格格式串，**不要**带结尾 '\n'
 *
 * 不做加锁：断言、栈溢出等 hook 里也会调用，那里拿不了互斥锁。环形缓冲的指针更新
 * 用关中断保护（临界区很短），所以中断与异常上下文里调用也是安全的。
 */
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/**
 * 把缓冲里攒的日志吐给 stdio。
 *
 * 主机没连上（stdio_usb_connected() 为假）时什么都不做：CDC 在 DTR 拉起前写什么都丢，
 * 不如原样留着等下一次。缓冲满了被丢掉的字节数会在这里以一行提示报出来。
 *
 * 只应由链路任务周期性地调用（单消费者），且 printf/stdio 必须已经就绪。
 *
 * @return 本次实际写出的字节数（0 表示没吐出去，可能没主机，也可能缓冲是空的）
 */
size_t log_drain(void);

#ifdef __cplusplus
}
#endif

#endif /* _LOG_OUT_H_ */
