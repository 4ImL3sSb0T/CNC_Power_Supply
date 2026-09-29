#ifndef _RESET_REASON_H_
#define _RESET_REASON_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file        reset_reason.h
 * @brief       复位原因取证：POWMAN 记录的复位来源 + 崩溃现场（HardFault/断言）跨复位标记
 *
 * 解决什么问题：USB 上跑协议时设备可能毫无征兆地重启，事后只能靠证据倒推。两条线索：
 *
 *   1. POWMAN_CHIP_RESET —— 硬件记录"上一次复位是谁引起的"（POR / BOR / 看门狗 / 毛刺…）。
 *      POWMAN 在 always-on 域，掉电复位也留得下。但它分辨不出软件崩溃：
 *      HardFault 是内核异常，不会引起芯片级复位，寄存器里不会有任何位被置上。
 *
 *   2. watchdog scratch[8] —— 同样是 always-on 域的普通读写寄存器，跨复位保留。
 *      崩溃/断言现场把标记写进去，重启后读出来，就能补上 POWMAN 看不到的那一类。
 *
 * 用法：stdio 可用之后调一次 reset_reason_report() 即可（HardFault 钩子是链接期覆盖的
 * 弱符号，不需要运行时安装）。
 */

/* 崩溃标记。数值会原样进 scratch，改数值会让上一次复位留下的标记被认成未知 */
typedef enum {
    RESET_MARK_NONE = 0,
    RESET_MARK_HARDFAULT = 1,        /* HardFault：内存越界、非法指令、总线错误… */
    RESET_MARK_ASSERT = 2,           /* configASSERT 失败 */
    RESET_MARK_STACK_OVERFLOW = 3,   /* FreeRTOS 检测到任务栈溢出 */
    RESET_MARK_MALLOC_FAILED = 4,    /* FreeRTOS 堆耗尽 */
} reset_mark_t;

/**
 * 记录崩溃现场。只写几个寄存器：不打印、不分配内存、不依赖任何已初始化的东西 ——
 * 调用点可能是栈已溢出或堆已耗尽的上下文，那里任何复杂动作都是二次伤害。
 *
 * @param why     崩溃类型
 * @param where   出处的字符串常量（__FILE__ 或任务名），没有就传 NULL。
 *                必须是 flash 里的常量字符串 —— 指针要跨复位继续有效
 * @param detail  附加数值（断言行号 / CFSR 等），没有就传 0
 */
void reset_reason_mark(reset_mark_t why, const char *where, uint32_t detail);

/**
 * 读出并打印上次复位的原因，随后清掉标记。
 *
 * 必须清：scratch 跨复位保留，不清的话下一次复位读到的是上一次的现场，
 * 会把人引到完全错误的方向。POR（真掉电）后 scratch 内容未定义，
 * 靠 magic 校验区分"本固件写过"和"上电随机值"。
 */
void reset_reason_report(void);

#ifdef __cplusplus
}
#endif

#endif /* _RESET_REASON_H_ */
