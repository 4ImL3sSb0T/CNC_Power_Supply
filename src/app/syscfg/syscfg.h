/**
 * @file        syscfg.h
 * @brief       持久化配置：字段表、默认值、范围校验、上电应用策略
 *
 * 配置是**预设**，不是实时状态的镜像 —— 实时设定值在 pcb_ctrl / psu_link 手里。
 * 默认值的权威来源是 board_config.h 与 psu_link.h 的宏，这里不重复写死。
 *
 * 上电应用策略（安全优先，见 main.c 头注释的第一原则）：
 *   - 遥测周期：自动应用（非安全项，且上位机一连上就能覆盖）
 *   - 看门狗超时：只作为"下次 SET_REMOTE 的建议值"，**绝不自动武装**。否则上电 200ms
 *     后主机还没连上，看门狗就自触发一次急停，白白在日志里造一条故障记录
 *   - 电压/限流预设、输出开关：**不碰硬件**。上电必须停在安全态（CE# 关断 + 双 PWM 0%），
 *     预设只通过 CFG_GET 回显，由上位机决定要不要下发（CLI 的 `cfg restore`）
 *
 * 线程约定：本模块的状态只在 psu_link_task 里改（CFG_* 命令与应用都在那个任务上下文），
 * 所以内部不需要锁。syscfg_ensure_loaded() / syscfg_tick() 也只应由它每轮调用。
 */

#ifndef SYSCFG_H
#define SYSCFG_H

#include <stdbool.h>
#include "lib/tools/common_def.h"

/* 设定值改动后延迟这么久才落盘：PWM 扫描是几十毫秒内连着改十几次，
 * 每次都写 Flash 既慢又费寿命，去抖后整段扫描只写一次 */
#define SYSCFG_SETPOINT_SAVE_MS     2000u

typedef struct {
    u16 telem_period_ms;
    u16 wd_timeout_ms;
    u16 vout_permille;              /* 预设电压设定（占空比‰） */
    u16 ilim_permille;              /* 预设限流设定（占空比‰） */
    u32 boot_count;                 /* 累计上电次数 */
} syscfg_t;

/* 幂等。首次真正加载完成时返回 true（调用方据此把要应用的默认值取走用一次），
 * 之前若干轮调用返回 false（文件系统还没挂载好）。加载时会顺手写一条 BOOT 日志。 */
bool        syscfg_ensure_loaded(void);

/* 当前有效配置 + PSU_CFG_FLAG_* 标志。未加载完返回 EXIT_BUSY */
exit_code_t syscfg_get(syscfg_t *out, u8 *flags);

/* CFG_SET：field 见 psu_cfg_field_t，value 用 mV/mA 或 ms。
 * *effected 回生效值（VOUT/ILIM 回换算后的占空比‰，其余回原值）。 */
exit_code_t syscfg_set_field(u8 field, u16 value, u16 *effected);

/* 恢复默认值并落盘（boot_count 是计数器，不重置） */
exit_code_t syscfg_reset(void);

/* 通知"某个设定值被改了"，去抖 SYSCFG_SETPOINT_SAVE_MS 后自动落盘 */
void        syscfg_note_setpoint(u8 field, u16 permille);

/* 去抖计时。由 psu_link_task 每轮调用 */
void        syscfg_tick(void);

#endif /* SYSCFG_H */
