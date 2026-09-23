/**
 * @file        pcb_test.c
 * @brief       数控电源v1 验证板测试序列（屏幕 + 串口同步输出）
 *
 * 测试项（顺序执行）：
 *   1 SELF-IMU    测试台外设自检：IMU(0x23) 读版本 + 加速度
 *   2 SAFE-STATE  上电安全态读回 + 关断态 PG 节点电压
 *   3 PG-ENABLE   使能后 PG 建立时间（需要 VIN 上电；超时提示查 VIN）
 *   4 PG-DISABLE  关断后 PG 撤销时间
 *   5 PG-LEVELS   PG 节点 good/fault 电平 vs MCU 逻辑阈值（量化 LED3 压降）
 *   6 PWM-SWEEP   输出电压设定扫描 0/25/50/75/100%（3.90→23.40V）
 *   7 IPWM-SWEEP  输出电流限扫描 0/25/50/75/100%（0→5.04A，需电子负载核对）
 * 结束/急停一律回安全态：CE# 关断、PWM 0%、IPWM 0%。
 *
 * 按键（KEY0）：单击=跑序列，双击=手动步进电压设定（会开输出），
 *              长按=急停回安全态。
 */

#include "app/pcb_test/pcb_test.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "config/board_config.h"
#include "bsp/power/pcb_ctrl.h"
#include "bsp/input/key.h"
#include "lib/tools/vec_math.h"
#include "driver/imu/imu.h"

typedef test_result_t (*test_fn_t)(char *detail, u32 n);

typedef struct {
    const char *name;
    test_fn_t fn;
} test_case_t;

static test_status_t s_status;
static SemaphoreHandle_t s_lock;
static volatile u8 s_cmd_run;
static volatile u8 s_cmd_stop;
static volatile u8 s_cmd_manual;

static u16 s_pg_fault_mv;       /* 关断态 PG 节点电压 */
static u16 s_pg_good_mv;        /* 使能态 PG 节点电压 */

static const u16 s_sweep_steps[5] = { 0, 250, 500, 750, 1000 };

/* ---------------- 基础工具 ---------------- */

static void status_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void status_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* 把 pcb_ctrl 的实时读数刷进快照（只在 test_task 调用） */
static void refresh_live(void)
{
    status_lock();
    s_status.pwm_permille = pcb_ctrl_pwm_duty();
    s_status.ipwm_permille = pcb_ctrl_ipwm_duty();
    s_status.ce_on = pcb_ctrl_ce_on();
    s_status.pg_mv = pcb_ctrl_pg_node_mv();
#if PCB_TEST_VOUT_SENSE_ENABLE
    s_status.vout_mv = pcb_ctrl_read_vout_mv();
#endif
    status_unlock();
}

static void fmt_mv(char *dst, u32 n, u16 mv)
{
    snprintf(dst, n, "%u.%02u", (unsigned)(mv / 1000u), (unsigned)((mv % 1000u) / 10u));
}

/* ---------------- 测试项 ---------------- */

/* 1. IMU 在线（测试台外设，不是板上器件） */
static test_result_t item_self_imu(char *d, u32 n)
{
    vec3f accel;

    if (imu_probe_version() == EXIT_OK && imu_get_accel(&accel) == EXIT_OK) {
        snprintf(d, n, "0x23 ok");
        return TRES_PASS;
    }
    snprintf(d, n, "no resp");
    return TRES_FAIL;
}

/* 2. 上电安全态：CE# 关断、双 PWM 0%，记录关断态 PG 节点电压 */
static test_result_t item_safe_state(char *d, u32 n)
{
    u16 mv = pcb_ctrl_pg_node_mv();

    s_pg_fault_mv = mv;
    if (!pcb_ctrl_ce_on() && pcb_ctrl_pwm_duty() == 0 && pcb_ctrl_ipwm_duty() == 0) {
        char v[8];
        fmt_mv(v, sizeof(v), mv);
        snprintf(d, n, "PG=%sV", v);
        return TRES_PASS;
    }
    snprintf(d, n, "not safe!");
    return TRES_FAIL;
}

/* 3. 使能后 PG 建立：5A 限流 + 3.9V 设定起步，测 FAULT→GOOD 时间 */
static test_result_t item_pg_enable(char *d, u32 n)
{
    u32 t0;
    u32 dt_ms;
    char v[8];

    pcb_ctrl_set_ipwm_duty(1000);
    pcb_ctrl_set_pwm_duty(0);
    vTaskDelay(pdMS_TO_TICKS(50));
    pcb_ctrl_ce(true);

    t0 = time_us_32();
    while (pcb_ctrl_pg_state() != PG_GOOD) {
        dt_ms = (time_us_32() - t0) / 1000u;
        if (s_cmd_stop) {
            snprintf(d, n, "aborted");
            return TRES_SKIP;
        }
        if (dt_ms > PCB_PG_TIMEOUT_MS) {
            snprintf(d, n, "timeout!chk VIN");
            return TRES_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    dt_ms = (time_us_32() - t0) / 1000u;

    vTaskDelay(pdMS_TO_TICKS(200));
    s_pg_good_mv = pcb_ctrl_pg_node_mv();
    refresh_live();
    fmt_mv(v, sizeof(v), s_pg_good_mv);
    snprintf(d, n, "t=%ums H=%s", (unsigned)dt_ms, v);
    return TRES_PASS;
}

/* 4. 关断后 PG 撤销：测 GOOD→FAULT 时间，记录 fault 态电压 */
static test_result_t item_pg_disable(char *d, u32 n)
{
    u32 t0;
    u32 dt_ms;
    char v[8];

    pcb_ctrl_ce(false);
    t0 = time_us_32();
    while (pcb_ctrl_pg_state() != PG_FAULT) {
        dt_ms = (time_us_32() - t0) / 1000u;
        if (s_cmd_stop) {
            snprintf(d, n, "aborted");
            return TRES_SKIP;
        }
        if (dt_ms > PCB_PG_OFF_TIMEOUT_MS) {
            snprintf(d, n, "still good");
            return TRES_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    dt_ms = (time_us_32() - t0) / 1000u;

    vTaskDelay(pdMS_TO_TICKS(50));
    s_pg_fault_mv = pcb_ctrl_pg_node_mv();
    refresh_live();
    fmt_mv(v, sizeof(v), s_pg_fault_mv);
    snprintf(d, n, "t=%ums L=%s", (unsigned)dt_ms, v);
    return TRES_PASS;
}

/* 5. PG 节点电平质量：LED3 在读出回路里，fault 电平被抬到 ≈2.1V。
 *    超出 MCU VIL/VIH 阈值就给 WARN —— 数字读不可靠，本固件才用 ADC 读。 */
static test_result_t item_pg_levels(char *d, u32 n)
{
    char vh[8];
    char vl[8];

    if (s_pg_good_mv == 0 && s_pg_fault_mv == 0) {
        snprintf(d, n, "no data");
        return TRES_SKIP;
    }
    fmt_mv(vh, sizeof(vh), s_pg_good_mv);
    fmt_mv(vl, sizeof(vl), s_pg_fault_mv);
    snprintf(d, n, "H=%s L=%s", vh, vl);

    if (s_pg_good_mv >= PCB_MCU_VIH_MIN_MV && s_pg_fault_mv <= PCB_MCU_VIL_MAX_MV) {
        return TRES_PASS;
    }
    return TRES_WARN;
}

/* 6. 输出电压设定扫描：0/25/50/75/100% → 3.90–23.40V。
 *    启用外接分压时逐点比对 ±5%；否则屏显设定值供万用表核对。 */
static test_result_t item_pwm_sweep(char *d, u32 n)
{
    u8 i;
    u8 ok_cnt = 0;

    pcb_ctrl_set_ipwm_duty(1000);
    vTaskDelay(pdMS_TO_TICKS(50));
    pcb_ctrl_ce(true);

    for (i = 0; i < 5; i++) {
        if (s_cmd_stop) {
            snprintf(d, n, "aborted");
            return TRES_SKIP;
        }
        pcb_ctrl_set_pwm_duty(s_sweep_steps[i]);
        vTaskDelay(pdMS_TO_TICKS(PCB_SWEEP_SETTLE_MS));
        refresh_live();

#if PCB_TEST_VOUT_SENSE_ENABLE
        {
            u16 set_mv = pcb_ctrl_vout_setpoint_mv();
            u16 meas_mv = pcb_ctrl_read_vout_mv();
            u32 err_mv = (meas_mv > set_mv) ? (meas_mv - set_mv) : (set_mv - meas_mv);

            if (err_mv * 100u <= (u32)set_mv * PCB_VOUT_SENSE_TOLERANCE_PCT) {
                ok_cnt++;
            }
        }
#endif
        printf("[PWM-SWEEP] D=%u%% VSET=%umV PG=%umV\n",
               (unsigned)s_sweep_steps[i] / 10u,
               (unsigned)pcb_ctrl_vout_setpoint_mv(),
               (unsigned)pcb_ctrl_pg_node_mv());
    }
    pcb_ctrl_set_pwm_duty(0);
    refresh_live();

#if PCB_TEST_VOUT_SENSE_ENABLE
    snprintf(d, n, "VOUT %u/5 ok", (unsigned)ok_cnt);
    return (ok_cnt == 5) ? TRES_PASS : TRES_FAIL;
#else
    (void)ok_cnt;
    snprintf(d, n, "5 pts 3.9-23.4V");
    return TRES_MANUAL;
#endif
}

/* 7. 输出电流限扫描：0/25/50/75/100% → 0–5.04A，需电子负载人工核对截流点。
 *    D=0 时限流为 0，输出会塌掉，属预期现象。 */
static test_result_t item_ipwm_sweep(char *d, u32 n)
{
    u8 i;

    pcb_ctrl_set_pwm_duty(0);
    vTaskDelay(pdMS_TO_TICKS(50));
    pcb_ctrl_ce(true);

    for (i = 0; i < 5; i++) {
        if (s_cmd_stop) {
            snprintf(d, n, "aborted");
            return TRES_SKIP;
        }
        pcb_ctrl_set_ipwm_duty(s_sweep_steps[i]);
        vTaskDelay(pdMS_TO_TICKS(PCB_IPWM_SETTLE_MS));
        refresh_live();
        printf("[IPWM-SWEEP] D=%u%% ILIM=%umA\n",
               (unsigned)s_sweep_steps[i] / 10u,
               (unsigned)pcb_ctrl_ilim2_setpoint_ma());
    }
    pcb_ctrl_set_ipwm_duty(1000);

    snprintf(d, n, "5 pts 0-5.04A");
    return TRES_MANUAL;
}

static const test_case_t s_cases[TEST_ITEM_COUNT] = {
    { "SELF-IMU",   item_self_imu },
    { "SAFE-STATE", item_safe_state },
    { "PG-ENABLE",  item_pg_enable },
    { "PG-DISABLE", item_pg_disable },
    { "PG-LEVELS",  item_pg_levels },
    { "PWM-SWEEP",  item_pwm_sweep },
    { "IPWM-SWEEP", item_ipwm_sweep },
};

static const char *result_name(test_result_t r)
{
    switch (r) {
    case TRES_RUNNING: return "RUN";
    case TRES_PASS:    return "PASS";
    case TRES_FAIL:    return "FAIL";
    case TRES_WARN:    return "WARN";
    case TRES_SKIP:    return "SKIP";
    case TRES_MANUAL:  return "MANL";
    default:           return "-";
    }
}

/* ---------------- 序列执行 ---------------- */

static void run_sequence(void)
{
    u8 i;

    s_cmd_run = 0;
    s_cmd_manual = 0;

    status_lock();
    for (i = 0; i < TEST_ITEM_COUNT; i++) {
        s_status.items[i].result = TRES_PENDING;
        s_status.items[i].detail[0] = '\0';
        s_status.items[i].elapsed_ms = 0;
    }
    s_status.run_state = RUN_RUNNING;
    s_status.manual_mode = 0;
    status_unlock();

    printf("==== PCB 测试开始 ====\n");

    for (i = 0; i < TEST_ITEM_COUNT; i++) {
        char detail[TEST_DETAIL_LEN];
        test_result_t result;
        u32 t0;
        u32 dt_ms;

        if (s_cmd_stop) {
            u8 j;
            status_lock();
            for (j = i; j < TEST_ITEM_COUNT; j++) {
                s_status.items[j].result = TRES_SKIP;
                snprintf(s_status.items[j].detail, TEST_DETAIL_LEN, "aborted");
            }
            status_unlock();
            break;
        }

        status_lock();
        s_status.cur_index = i;
        s_status.items[i].result = TRES_RUNNING;
        status_unlock();

        printf("[%u/%u] %s ...\n", (unsigned)(i + 1), (unsigned)TEST_ITEM_COUNT, s_cases[i].name);

        t0 = time_us_32();
        detail[0] = '\0';
        result = s_cases[i].fn(detail, sizeof(detail));
        dt_ms = (time_us_32() - t0) / 1000u;

        status_lock();
        s_status.items[i].result = result;
        snprintf(s_status.items[i].detail, TEST_DETAIL_LEN, "%s", detail);
        s_status.items[i].elapsed_ms = dt_ms;
        status_unlock();

        printf("[%u/%u] %s -> %s %s (%ums)\n",
               (unsigned)(i + 1), (unsigned)TEST_ITEM_COUNT, s_cases[i].name,
               result_name(result), detail, (unsigned)dt_ms);
    }

    pcb_ctrl_safe_state();
    refresh_live();

    status_lock();
    s_status.run_state = s_cmd_stop ? RUN_ABORTED : RUN_DONE;
    status_unlock();
    printf("==== PCB 测试结束 (%s) ====\n", s_cmd_stop ? "已急停" : "完成");
}

/* 手动步进：5 个电压设定点循环，首次进入会开输出（IPWM=100%） */
static void manual_step(void)
{
    static u8 idx = 0;

    if (!s_status.manual_mode) {
        idx = 0;
        status_lock();
        s_status.manual_mode = 1;
        status_unlock();
        pcb_ctrl_set_ipwm_duty(1000);
        pcb_ctrl_ce(true);
    } else {
        idx = (u8)((idx + 1) % 5);
    }
    pcb_ctrl_set_pwm_duty(s_sweep_steps[idx]);
    refresh_live();

    printf("[MANUAL] D=%u%% VSET=%umV CE=ON\n",
           (unsigned)s_sweep_steps[idx] / 10u,
           (unsigned)pcb_ctrl_vout_setpoint_mv());
}

void pcb_test_task(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        if (s_cmd_run) {
            run_sequence();
            continue;
        }

        if (s_cmd_manual) {
            s_cmd_manual = 0;
            if (s_status.run_state != RUN_RUNNING) {
                manual_step();
            }
        }

        if (s_cmd_stop) {
            s_cmd_stop = 0;
            pcb_ctrl_safe_state();
            status_lock();
            s_status.manual_mode = 0;
            status_unlock();
            printf("[SAFE] 安全态：CE# 关断、PWM 0%%、IPWM 0%%\n");
        }

        refresh_live();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---------------- 按键 ---------------- */

static void on_key(Button *btn)
{
    switch (button_get_event(btn)) {
    case BTN_SINGLE_CLICK:
        s_cmd_run = 1;
        break;
    case BTN_DOUBLE_CLICK:
        s_cmd_manual = 1;
        break;
    case BTN_LONG_PRESS_START:
        s_cmd_stop = 1;
        break;
    default:
        break;
    }
}

void pcb_test_bind_keys(void)
{
    key_attach(KEY_ID_0, BTN_SINGLE_CLICK, on_key);
    key_attach(KEY_ID_0, BTN_DOUBLE_CLICK, on_key);
    key_attach(KEY_ID_0, BTN_LONG_PRESS_START, on_key);
}

/* ---------------- 初始化 / 快照 ---------------- */

exit_code_t pcb_test_init(void)
{
    u8 i;

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return EXIT_NO_MEMORY;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.run_state = RUN_IDLE;
    for (i = 0; i < TEST_ITEM_COUNT; i++) {
        s_status.items[i].name = s_cases[i].name;
        s_status.items[i].result = TRES_PENDING;
    }
    return EXIT_OK;
}

void pcb_test_get_status(test_status_t *out)
{
    if (out == NULL) {
        return;
    }
    status_lock();
    *out = s_status;
    status_unlock();
}
