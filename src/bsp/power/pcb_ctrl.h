/**
 * @file        pcb_ctrl.h
 * @brief       数控电源v1（SC8701 验证板）控制接口：CE# / PWM / IPWM / PG 采样
 *
 * H1 接线与板级常量见 config/board_config.h。
 * 安全态 = CE# 关断 + PWM 0%（设定 3.9V）+ IPWM 0%（限流 0A，无输出能力）。
 * 线程约定：全部 API 只在 test_task 上下文调用（内部 ADC 无锁，单任务持有）。
 */

#ifndef PCB_CTRL_H
#define PCB_CTRL_H

#include <stdbool.h>
#include "lib/tools/common_def.h"
#include "config/board_config.h"

typedef enum {
    PG_FAULT = 0,       /* 节点 ≤ PCB_PG_FAULT_MAX_MV：故障 / 未建立 */
    PG_UNKNOWN,         /* 滞回区间内，保持上次判定 */
    PG_GOOD             /* 节点 ≥ PCB_PG_GOOD_MIN_MV：输出已建立 */
} pcb_pg_state_t;

/* 最先调用：把 CE#/PWM/IPWM 立即置安全态，再配置 PWM 与 ADC */
exit_code_t pcb_ctrl_init(void);

/* 急停/收尾：CE# 关断、占空比归零 */
void pcb_ctrl_safe_state(void);

void pcb_ctrl_ce(bool enable);              /* true = 使能（CE# 拉低） */
bool pcb_ctrl_ce_on(void);

void pcb_ctrl_set_pwm_duty(u16 permille);   /* 输出电压设定 0–1000‰ */
void pcb_ctrl_set_ipwm_duty(u16 permille);  /* 输出电流限值 0–1000‰ */
u16 pcb_ctrl_pwm_duty(void);
u16 pcb_ctrl_ipwm_duty(void);

u16 pcb_ctrl_vout_setpoint_mv(void);        /* (1+5D)/6 × 23.4V */
u16 pcb_ctrl_ilim2_setpoint_ma(void);       /* D × 5.04A */

u16 pcb_ctrl_pg_node_mv(void);              /* H1.4 模拟电压（8 次平均） */
pcb_pg_state_t pcb_ctrl_pg_state(void);     /* 带滞回的 GOOD/FAULT 判定 */

#if PCB_TEST_VOUT_SENSE_ENABLE
u16 pcb_ctrl_read_vout_mv(void);            /* 外接分压实测 VOUT */
#endif

#endif
