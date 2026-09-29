#ifndef _LOG_OUT_H_
#define _LOG_OUT_H_

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file        log_out.h
 * @brief       设备日志输出：统一行前缀，便于上位机从复用同一条 CDC 的协议流里分离日志
 *
 * 背景：USB CDC 上协议帧与 printf 文本共用一条链路（见 app/psu_link/psu_link.h）。
 * 上位机靠 COBS 的 0x00 分块，把解不出合法帧的块当文本收下 —— 但块里既可能是真正的
 * 日志行，也可能是被日志打断的半截帧。给每行日志加固定前缀后，上位机可以精确地把
 * 两者分开：带前缀的进"设备日志"页，不带的当链路杂音。
 *
 * 约定：一行日志 = LOG_LINE_PREFIX + 正文 + '\n'，前缀落在行首。调用点不要自己带
 * 结尾换行，由 log_printf 统一补，否则一条日志会被拆成两行而只有首行带前缀。
 */

/* 与上位机 scripts/src/psu_host/protocol.py 的 LOG_LINE_PREFIX 必须一致 */
#define LOG_LINE_PREFIX "# "

/* 单条日志（含前缀与换行）的长度上限，超出的部分被截断。最长的一条是
 * app/syscfg 的就绪信息，约 90 字节，留一倍余量。 */
#define LOG_LINE_MAX 160

/**
 * 输出一行设备日志。
 *
 * @param fmt printf 风格格式串，**不要**带结尾 '\n'
 *
 * 不做加锁：断言、栈溢出等 hook 里也会调用，那里拿不了互斥锁。代价是多任务同时
 * 打日志时，两次输出的字节可能与协议帧交错，上位机按前缀识别时会把它当杂音。
 */
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* _LOG_OUT_H_ */
