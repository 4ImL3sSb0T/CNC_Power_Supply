#include "lib/tools/reset_reason.h"

#include <stdbool.h>

#include "hardware/structs/powman.h"
#include "hardware/structs/watchdog.h"
#include "lib/tools/log_out.h"
#include "pico/stdlib.h"

/* watchdog scratch 的槽位分配。8 个槽都没人用，取前 4 个：
 *   [0] magic  —— "本固件写过"的标志。上电后 scratch 是未定义值，不校验会误报
 *   [1] 崩溃类型
 *   [2] where 指针
 *   [3] detail
 * magic 最后写、最先清：它一置上就代表另外三个槽已经就位，读的人才敢信它们 */
#define RESET_MARK_MAGIC    0x52535431u     /* 'RST1' */
#define SLOT_MAGIC          0
#define SLOT_WHY            1
#define SLOT_WHERE          2
#define SLOT_DETAIL         3

/* Cortex-M33 的 SCB 故障状态寄存器，地址固定（不引 CMSIS 头也能读）。
 * 总线错误、内存保护、用法错误三类信息都在 CFSR 里 */
#define SCB_CFSR            (*(volatile uint32_t *)0xE000ED28u)

/* 字符串常量所在的 XIP flash 区间。跨复位读回来的指针必须先过这道校验再解引用 */
#define XIP_FLASH_BASE      0x10000000u
#define XIP_FLASH_END       0x20000000u

void reset_reason_mark(reset_mark_t why, const char *where, uint32_t detail)
{
    watchdog_hw->scratch[SLOT_WHY] = (uint32_t)why;
    watchdog_hw->scratch[SLOT_WHERE] = (uint32_t)(uintptr_t)where;
    watchdog_hw->scratch[SLOT_DETAIL] = detail;
    watchdog_hw->scratch[SLOT_MAGIC] = RESET_MARK_MAGIC;
}

/* 覆盖 crt0.S 里的弱符号 isr_hardfault。默认实现是一条 bkpt：接了调试器就停在断点上，
 * 没接调试器会二次 HardFault 然后把内核锁死 —— 那条路径什么证据都不留。
 * 这里只做"存证据 + 停住"：不打印，因为异常上下文里栈可能已经坏了、stdio 也未必可用。 */
void isr_hardfault(void)
{
    reset_reason_mark(RESET_MARK_HARDFAULT, NULL, SCB_CFSR);
    for (;;) {
        tight_loop_contents();
    }
}

typedef struct {
    uint32_t    bit;
    const char *text;
} reset_bit_t;

/* POWMAN_CHIP_RESET 的全部 HAD_*_BITS。按"最可能是元凶"排而不是按位序，
 * 一眼看到的第一行就是最该怀疑的 */
static const reset_bit_t k_reset_bits[] = {
    { POWMAN_CHIP_RESET_HAD_BOR_BITS,                         "BOR 欠压复位（VIN 跌落）" },
    { POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS,               "电源毛刺" },
    { POWMAN_CHIP_RESET_HAD_POR_BITS,                         "POR 上电复位" },
    { POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS,                     "RUN 引脚被拉低" },
    { POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_PSM_BITS,          "看门狗复位 → PSM" },
    { POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS,       "看门狗复位 → swcore" },
    { POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS,       "看门狗复位 → powman" },
    { POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_ASYNC_BITS, "看门狗复位 → powman 异步" },
    { POWMAN_CHIP_RESET_HAD_RESCUE_BITS,                      "rescue 复位" },
    { POWMAN_CHIP_RESET_HAD_HZD_SYS_RESET_REQ_BITS,           "hazard debugger 系统复位" },
    { POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS,                "调试器 DP 复位请求" },
    { POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS,                   "swcore 掉电" },
};

static const char *mark_name(uint32_t why)
{
    switch (why) {
    case RESET_MARK_HARDFAULT:      return "HardFault（内存越界 / 非法指令 / 总线错误）";
    case RESET_MARK_ASSERT:         return "configASSERT 断言失败";
    case RESET_MARK_STACK_OVERFLOW: return "任务栈溢出";
    case RESET_MARK_MALLOC_FAILED:  return "FreeRTOS 堆耗尽";
    default:                        return "未知标记";
    }
}

void reset_reason_report(void)
{
    uint32_t chip_reset = powman_hw->chip_reset;
    uint32_t wd_reason = watchdog_hw->reason;
    bool any = false;
    size_t i;

    log_printf("复位来源：chip_reset=0x%08X，看门狗 reason=0x%X",
               (unsigned)chip_reset, (unsigned)wd_reason);

    for (i = 0; i < count_of(k_reset_bits); i++) {
        if (chip_reset & k_reset_bits[i].bit) {
            log_printf("  · %s", k_reset_bits[i].text);
            any = true;
        }
    }
    if (!any) {
        /* 这是重要信息而不是异常：HardFault 属于内核异常，不会引起芯片级复位，
         * POWMAN 自然一个位都不置 —— 所以真凶只能从下面的 scratch 标记里找 */
        log_printf("  · （POWMAN 没有置位：说明不是芯片级复位，软件崩溃看下面）");
    }

    if (watchdog_hw->scratch[SLOT_MAGIC] == RESET_MARK_MAGIC) {
        uint32_t why = watchdog_hw->scratch[SLOT_WHY];
        uint32_t where = watchdog_hw->scratch[SLOT_WHERE];
        uint32_t detail = watchdog_hw->scratch[SLOT_DETAIL];

        log_printf("上次死在这里：%s（detail=0x%X）", mark_name(why), (unsigned)detail);

        /* 指针来自崩溃现场，复位后必须校验区间再解引用 */
        if (where >= XIP_FLASH_BASE && where < XIP_FLASH_END) {
            log_printf("  位置：%s", (const char *)(uintptr_t)where);
        }
    }

    /* 必须清：scratch 跨复位保留，留着的话下次复位读到的是这一次的现场 */
    watchdog_hw->scratch[SLOT_MAGIC] = 0u;
}
