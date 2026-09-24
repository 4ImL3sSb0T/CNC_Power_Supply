/**
 * @file        pcb_test.h
 * @brief       PCB 测试序列：测试项模型、运行状态、命令队列、UI/上位机快照
 */

#ifndef PCB_TEST_H
#define PCB_TEST_H

#include <stdbool.h>
#include "lib/tools/common_def.h"

typedef enum {
    RUN_IDLE = 0,
    RUN_RUNNING,
    RUN_DONE,
    RUN_ABORTED
} test_run_state_t;

typedef enum {
    TRES_PENDING = 0,
    TRES_RUNNING,
    TRES_PASS,
    TRES_FAIL,
    TRES_WARN,
    TRES_SKIP,
    TRES_MANUAL        /* 设定正确，需万用表/电子负载人工核对 */
} test_result_t;

#define TEST_ITEM_COUNT     7
#define TEST_DETAIL_LEN     22

typedef struct {
    const char *name;
    test_result_t result;
    char detail[TEST_DETAIL_LEN];
    u32 elapsed_ms;
} test_item_t;

typedef struct {
    test_run_state_t run_state;
    u8 manual_mode;             /* 手动设定（本地双击或上位机设值） */
    u8 remote;                  /* 远程模式：按键只保留长按急停 */
    u8 cur_index;
    u16 pwm_permille;           /* 当前输出电压设定 */
    u16 ipwm_permille;          /* 当前电流限值设定 */
    u16 pg_mv;                  /* PG 节点电压（关断态约 2.1V，建立后 3.3V） */
    u16 vout_mv;                /* 外接分压实测，0 = 未启用/未接 */
    bool ce_on;
    test_item_t items[TEST_ITEM_COUNT];
} test_status_t;

/* ---------------------------------------------------------------- */
/* 命令队列：本地按键与上位机（app/psu_link）都往同一条队列投，           */
/* pcb_test_task 串行执行，保证 pcb_ctrl 始终只有一个写者。            */
/* ---------------------------------------------------------------- */
typedef enum {
    PCB_TEST_CMD_SET_VOUT = 0,  /* arg = 输出电压设定占空比 0–1000‰ */
    PCB_TEST_CMD_SET_ILIM,      /* arg = 电流限值设定占空比 0–1000‰ */
    PCB_TEST_CMD_SET_OUTPUT,    /* arg = 0 关 / 1 开（CE#） */
    PCB_TEST_CMD_MANUAL_STEP,   /* 手动步进电压设定（等效双击） */
    PCB_TEST_CMD_RUN,           /* 跑测试序列（等效单击） */
    PCB_TEST_CMD_STOP,          /* 中止测试序列 */
    PCB_TEST_CMD_SAFE,          /* 急停回安全态（等效长按，会打断执行中的测试项） */
    PCB_TEST_CMD_REMOTE,        /* arg = 0 退出 / 1 进入远程模式 */
} pcb_test_cmd_op_t;

#define PCB_TEST_CMD_QUEUE_LEN  8

typedef struct {
    u8 op;
    u16 arg;
} pcb_test_cmd_t;

exit_code_t pcb_test_init(void);
void pcb_test_bind_keys(void);
void pcb_test_task(void *pvParameters);
void pcb_test_get_status(test_status_t *out);

/* 从任意任务上下文投递命令（非 ISR）。队列满返回 EXIT_BUSY，
 * 未初始化返回 EXIT_NOT_INITIALIZED。命令是异步执行的：返回 EXIT_OK 只表示已受理，
 * 效果要看紧随其后的状态快照。 */
exit_code_t pcb_test_post_cmd(pcb_test_cmd_op_t op, u16 arg);

#endif
