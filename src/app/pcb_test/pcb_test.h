/**
 * @file        pcb_test.h
 * @brief       PCB 测试序列：测试项模型、运行状态、按键命令、UI 快照
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
    u8 manual_mode;             /* 手动步进输出电压设定 */
    u8 cur_index;
    u16 pwm_permille;           /* 当前输出电压设定 */
    u16 ipwm_permille;          /* 当前电流限值设定 */
    u16 pg_mv;                  /* PG 节点电压（关断态约 2.1V，建立后 3.3V） */
    u16 vout_mv;                /* 外接分压实测，0 = 未启用/未接 */
    bool ce_on;
    test_item_t items[TEST_ITEM_COUNT];
} test_status_t;

exit_code_t pcb_test_init(void);
void pcb_test_bind_keys(void);
void pcb_test_task(void *pvParameters);
void pcb_test_get_status(test_status_t *out);

#endif
