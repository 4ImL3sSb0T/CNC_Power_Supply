/**
 * @file        psu_link.c
 * @brief       上位机通信链路实现（USB CDC 收帧 → 命令 → 测试台；遥测/事件上报）
 *
 * 数据流（全部在 psu_link_task 里串行完成）：
 *
 *   tud_cdc_read ──► 逐字节分帧（0x00 分隔）──► COBS 解码 ──► CRC 校验
 *                                                            │
 *                          ┌─────────────────────────────────┘
 *                          ▼
 *                   命令分派 ──► pcb_test_post_cmd() ──► pcb_test_task ──► SC8701 控制脚
 *                          │                                  │
 *                          └──► ACK / INFO / STATUS           │
 *                                                             ▼
 *            遥测与事件 ◄── 差分 pcb_test_get_status() 快照 ◄──┘
 *
 * 为什么命令不在这里直接写控制脚：pcb_ctrl 的线程约定是"只在 test_task 上下文调用"
 * （内部 ADC 无锁）。所以这里只 post 命令，动作由 pcb_test_task 执行；ACK 表示
 * "已受理"，真正的效果在紧随其后的 TELEM 里能看到。
 */

#include "app/psu_link/psu_link.h"

#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "tusb.h"                       /* tud_cdc_*：TinyUSB 原生读，绕过 stdio 文本层 */
#include "FreeRTOS.h"
#include "task.h"

#include "app/pcb_test/pcb_test.h"
#include "app/syscfg/syscfg.h"
#include "bsp/power/pcb_ctrl.h"
#include "config/board_config.h"
#include "lib/proto/psu_proto.h"
#include "lib/tools/log_out.h"
#include "lib/tools/reset_reason.h"
#include "service/pstore/pstore.h"

/* ---------------- 链路状态（只在 link 任务里读写） ---------------- */

static u8  s_rx_block[PSU_PROTO_MAX_BLOCK]; /* 当前 COBS 块累积（不含 0x00 分隔符） */
static u32 s_rx_len;

static u32 s_telem_period_ms;               /* 0 = 停止上报 */
static u32 s_telem_next_ms;

static u32 s_wd_timeout_ms;                 /* 0 = 未武装 */
static u32 s_wd_last_ms;                    /* 最近一次收到主机有效帧的时刻 */
static u32 s_wd_fired_ms;                   /* 最近一次看门狗超时的窗口值，写日志用 */

static test_status_t s_prev;                /* 上一次轮询到的快照，用于差分出事件 */
static u8 s_prev_valid;

/* 主机是否显式设过遥测周期：设过就不再拿持久化配置里的默认值去覆盖它 */
static u8 s_telem_from_host;

/* 上次复位的原因是否已经报过。只能等主机连上再报：CDC 在 DTR 拉起前写什么都丢，
 * 而设备重启后 USB 重新枚举要几百毫秒，main() 里那几条启动日志必然赶不上 */
static u8 s_reset_reported;

/* ---------------- 工具 ---------------- */

/* 单调毫秒时基：调度器起来之前也有效（xTaskGetTickCount 在调度器前无意义） */
static u32 now_ms(void)
{
    return (u32)to_ms_since_boot(get_absolute_time());
}

/* 判定"设备正忙"：测试序列运行中不接受设定值/输出开关，急停与停止不受限 */
static bool test_busy(void)
{
    test_status_t snap;
    pcb_test_get_status(&snap);
    return snap.run_state == RUN_RUNNING;
}

/* PG 节点 → 状态：与 ui.c 用同一组阈值（带滞回的判定在 pcb_ctrl 内部，这里只做显示级判定） */
static u8 pg_state_from_mv(u16 mv)
{
    if (mv >= PCB_PG_GOOD_MIN_MV) {
        return (u8)PG_GOOD;
    }
    if (mv <= PCB_PG_FAULT_MAX_MV) {
        return (u8)PG_FAULT;
    }
    return (u8)PG_UNKNOWN;
}

/* ---------------- 发送 ---------------- */

/**
 * 发一帧。只在 link 任务上下文调用（tx 缓冲在栈上，无共享状态）。
 *
 * 先等 CDC TX FIFO 腾出整帧的空间再一次性写出：FIFO 只有 64 字节，这里链路上一帧
 * 最长 56 字节（含前后分隔符），写满一次就够；中途插入的 printf 会把帧劈成两半，
 * 对端只能丢帧。主机没打开串口（DTR 未拉低）时直接不写。
 */
static exit_code_t link_send(u8 cmd, u8 seq, const u8 *payload, u8 len)
{
    u8 tx[PSU_PROTO_MAX_ENCODED + 1u];      /* +1 给前置分隔符 */
    i32 total;
    u32 waited = 0;

    if (!tud_cdc_connected()) {
        return EXIT_FAIL;
    }

    /* 前置 0x00：printf 日志也走这条 CDC，而日志文本里没有 0x00。少了前置分隔符，
     * "一行日志 + 紧跟的帧"会粘成同一个块，整块 CRC 失败会连带把帧丢掉；
     * 有前置分隔符，日志在帧之前就作为独立文本块被主机切出去，帧本身完好。 */
    tx[0] = 0x00;
    total = psu_proto_encode(cmd, seq, payload, len, &tx[1], sizeof(tx) - 1u);
    if (total < 0) {
        return (exit_code_t)total;
    }
    total += 1;

    while (tud_cdc_write_available() < (u32)total) {
        tud_cdc_write_flush();
        if (waited >= PSU_LINK_TX_WAIT_MS) {
            return EXIT_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        waited++;
    }

    if (tud_cdc_write(tx, (u32)total) != (u32)total) {
        return EXIT_FAIL;
    }
    tud_cdc_write_flush();
    return EXIT_OK;
}

static void send_ack(u8 seq, u8 acked_cmd, exit_code_t code, u16 arg)
{
    u8 p[4];

    p[0] = acked_cmd;
    p[1] = (u8)(i8)code;                /* exit_code_t 负数按补码发，上位机按 int8 读 */
    psu_put_u16(&p[2], arg);
    (void)link_send(PSU_RSP_ACK, seq, p, sizeof(p));
}

static void send_pong(u8 seq, const psu_frame_t *req)
{
    u8 p[8];

    psu_put_u32(&p[0], (req->len >= 4u) ? psu_get_u32(req->payload) : 0u);
    p[4] = (u8)PSU_PROTO_VERSION;
    p[5] = 0u;                          /* role: 0 = 设备 */
    psu_put_u16(&p[6], 0u);
    (void)link_send(PSU_RSP_PONG, seq, p, sizeof(p));
}

/* 设备信息：板级常量由固件下发，上位机不重复写死任何一组换算常数 */
static void send_info(u8 seq)
{
    u8 p[16];
    u8 caps = PSU_CAP_WATCHDOG | PSU_CAP_ITEM_EVENTS | PSU_CAP_REMOTE | PSU_CAP_TELEM_PERIOD;

    if (PCB_TEST_VOUT_SENSE_ENABLE) {
        caps |= PSU_CAP_VOUT_SENSE;
    }
    /* 能力位跟着实际可用性走：文件系统没挂上就不宣称支持持久化，
     * 上位机据此干净地回落成纯 RAM 行为，而不是每条命令收一个错误码 */
    if (pstore_ready()) {
        caps |= PSU_CAP_CFG | PSU_CAP_STORE;
    }

    p[0] = (u8)PSU_PROTO_VERSION;
    p[1] = (u8)PSU_LINK_FW_MAJOR;
    p[2] = (u8)PSU_LINK_FW_MINOR;
    p[3] = (u8)TEST_ITEM_COUNT;
    psu_put_u16(&p[4], (u16)PCB_VOUT_SET_MV);
    psu_put_u16(&p[6], (u16)(PCB_VOUT_SET_MV / 6u));    /* D=0 时设定缩到 1/6 */
    psu_put_u16(&p[8], (u16)PCB_ILIM2_FULL_MA);
    psu_put_u16(&p[10], (u16)PCB_IIN_LIM_MA);
    psu_put_u16(&p[12], (u16)(PCB_PWM_FREQ_HZ / 1000u));
    p[14] = caps;
    p[15] = 0u;
    (void)link_send(PSU_RSP_INFO, seq, p, sizeof(p));
}

/* TELEM 与 STATUS 共用的 16 字节实时块 */
static void build_live_block(u8 *p, const test_status_t *s)
{
    u8 flags = 0;

    if (s->ce_on) {
        flags |= PSU_TELEM_FLAG_CE_ON;
    }
    if (s->manual_mode) {
        flags |= PSU_TELEM_FLAG_MANUAL;
    }
    if (s->remote) {
        flags |= PSU_TELEM_FLAG_REMOTE;
    }
    if (s_wd_timeout_ms != 0u) {
        flags |= PSU_TELEM_FLAG_WD_ARMED;
    }
    if (s->vout_mv != 0u) {
        flags |= PSU_TELEM_FLAG_VOUT_VALID;
    }

    p[0] = flags;
    p[1] = (u8)s->run_state;
    p[2] = s->cur_index;
    p[3] = pg_state_from_mv(s->pg_mv);
    psu_put_u16(&p[4], s->pwm_permille);
    psu_put_u16(&p[6], s->ipwm_permille);
    psu_put_u16(&p[8], s->pg_mv);
    psu_put_u16(&p[10], s->vout_mv);
    psu_put_u32(&p[12], now_ms());
}

static void send_telemetry(u8 seq, const test_status_t *s, bool with_items)
{
    u8 p[24];
    u8 i;

    build_live_block(p, s);
    if (with_items) {
        for (i = 0; i < TEST_ITEM_COUNT; i++) {
            p[16 + i] = (u8)s->items[i].result;
        }
        p[16 + TEST_ITEM_COUNT] = 0u;
        (void)link_send(PSU_RSP_STATUS, seq, p, (u8)(17u + TEST_ITEM_COUNT));
    } else {
        (void)link_send(PSU_RSP_TELEM, seq, p, 16u);
    }
}

/* 测试项状态变化（开始/出结果） */
static void send_item(u8 idx, u8 phase, const test_item_t *it)
{
    u8 p[PSU_ITEM_PAYLOAD_LEN];
    u8 i;

    memset(p, 0, sizeof(p));
    p[0] = phase;
    p[1] = idx;
    p[2] = (u8)it->result;
    p[3] = 0u;
    psu_put_u32(&p[4], it->elapsed_ms);
    if (it->name != NULL) {
        for (i = 0; i < (PSU_ITEM_NAME_LEN - 1u) && it->name[i] != '\0'; i++) {
            p[8 + i] = (u8)it->name[i];
        }
    }
    for (i = 0; i < (PSU_ITEM_DETAIL_LEN - 1u) && it->detail[i] != '\0'; i++) {
        p[8 + PSU_ITEM_NAME_LEN + i] = (u8)it->detail[i];
    }
    (void)link_send(PSU_RSP_ITEM, 0, p, (u8)sizeof(p));
}

static void send_event(u8 event, const u8 *data, u8 len)
{
    u8 p[3];

    p[0] = event;
    if (len > (u8)(sizeof(p) - 1u)) {
        return;
    }
    if (len != 0u && data != NULL) {
        memcpy(&p[1], data, len);
    }
    (void)link_send(PSU_RSP_EVENT, 0, p, (u8)(1u + len));
}

/* ---------------- 持久化配置 / 日志 ---------------- */

/* 配置：给上位机的是语义字段，不是设备上的落盘布局 */
static void send_cfg(u8 seq)
{
    syscfg_t cfg;
    u8 flags = 0u;
    u8 p[16];

    if (syscfg_get(&cfg, &flags) != EXIT_OK) {
        send_ack(seq, (u8)PSU_CMD_CFG_GET, EXIT_BUSY, 0);
        return;
    }

    p[0] = 1u;                          /* 配置结构版本 */
    p[1] = flags;
    psu_put_u16(&p[2], cfg.telem_period_ms);
    psu_put_u16(&p[4], cfg.wd_timeout_ms);
    psu_put_u16(&p[6], cfg.vout_permille);
    psu_put_u16(&p[8], cfg.ilim_permille);
    psu_put_u32(&p[10], cfg.boot_count);
    psu_put_u16(&p[14], 0u);            /* reserved */
    (void)link_send(PSU_RSP_CFG, seq, p, sizeof(p));
}

static void handle_cfg_set(u8 seq, const psu_frame_t *f)
{
    u16 effected = 0u;
    exit_code_t rc;

    if (f->len != 3u) {
        send_ack(seq, (u8)PSU_CMD_CFG_SET, EXIT_INVALID_PARAM, 0);
        return;
    }

    rc = syscfg_set_field(f->payload[0], psu_get_u16(&f->payload[1]), &effected);
    if (rc == EXIT_OK && f->payload[0] == (u8)PSU_CFG_F_TELEM_PERIOD) {
        /* 遥测周期是非安全项：改完立即生效，等效 SET_TELEM */
        s_telem_period_ms = effected;
        s_telem_next_ms = now_ms() + effected;
        s_telem_from_host = 1u;
    }
    /* 落盘是异步的：这里只是"已受理"，CFG.flags 的 DIRTY 位清零才算真写进去 */
    send_ack(seq, (u8)PSU_CMD_CFG_SET, rc, effected);
}

static void send_log_info(u8 seq)
{
    pstore_info_t info;
    u8 p[12];

    if (pstore_log_info(&info) != EXIT_OK) {
        send_ack(seq, (u8)PSU_CMD_LOG_INFO, EXIT_NOT_SUPPORTED, 0);
        return;
    }

    p[0] = info.rec_size;
    p[1] = info.seg_count;
    psu_put_u16(&p[2], info.seg_size);
    psu_put_u32(&p[4], info.total);
    psu_put_u32(&p[8], info.first_index);
    (void)link_send(PSU_RSP_LOGINFO, seq, p, sizeof(p));
}

/* 一次最多取两条（48 字节 payload 上限）。返回 0 条表示已经读到末尾 */
static void send_log_read(u8 seq, const psu_frame_t *f)
{
    pstore_rec_t rec[PSU_LOG_READ_MAX];
    u8 got = 0u;
    u8 p[1u + PSU_LOG_READ_MAX * PSU_LOG_REC_SIZE];
    exit_code_t rc;
    u8 i;

    if (f->len != 5u) {
        send_ack(seq, (u8)PSU_CMD_LOG_READ, EXIT_INVALID_PARAM, 0);
        return;
    }

    rc = pstore_log_read(psu_get_u32(&f->payload[0]), f->payload[4], rec, &got);
    if (rc != EXIT_OK) {
        send_ack(seq, (u8)PSU_CMD_LOG_READ, rc, 0);
        return;
    }

    p[0] = got;
    for (i = 0u; i < got; i++) {
        pstore_rec_pack(&rec[i], &p[1u + (u32)i * PSU_LOG_REC_SIZE]);
    }
    (void)link_send(PSU_RSP_LOGDATA, seq, p, (u8)(1u + (u32)got * PSU_LOG_REC_SIZE));
}

/* ---------------- 快照差分 → 事件/测试项上报 ---------------- */

static void poll_snapshot(void)
{
    test_status_t snap;
    u8 i;

    pcb_test_get_status(&snap);

    if (s_prev_valid) {
        if (snap.run_state != s_prev.run_state) {
            if (snap.run_state == RUN_RUNNING) {
                u8 d = (u8)TEST_ITEM_COUNT;
                send_event(PSU_EV_TEST_START, &d, 1);
                log_printf("[LINK] 测试序列开始");
            } else if (snap.run_state == RUN_DONE || snap.run_state == RUN_ABORTED) {
                u8 d = (u8)snap.run_state;
                send_event(PSU_EV_TEST_END, &d, 1);
                log_printf("[LINK] 测试序列结束（%s）", (snap.run_state == RUN_DONE) ? "完成" : "中止");
            }
        }

        if (snap.remote != s_prev.remote) {
            u8 d = snap.remote ? 1u : 0u;
            send_event(PSU_EV_REMOTE, &d, 1);
            if (!snap.remote) {
                /* 本地长按急停或看门狗把远程模式退掉了：看门狗同时撤销 */
                s_wd_timeout_ms = 0u;
                log_printf("[LINK] 远程模式结束（本地接管）");
            }
        }

        /* 回安全态的差分。本地长按、主机急停、看门狗超时三条路都会让 safe_seq 自增，
         * 原因随快照带出来 —— 所以连完全不经链路的那条（本地长按）也能记进日志 */
        if (snap.safe_seq != s_prev.safe_seq) {
            pstore_rec_t rec;

            pstore_rec_init(&rec, (u8)PSTORE_REC_SAFE);
            rec.a = snap.safe_reason;
            rec.pwm_permille = snap.pwm_permille;
            rec.ipwm_permille = snap.ipwm_permille;
            rec.arg = (snap.safe_reason == (u8)TEST_SAFE_REASON_WATCHDOG) ? s_wd_fired_ms : 0u;
            (void)pstore_log_append(&rec);
            log_printf("[LINK] 已记录回安全态事件（原因 %u）", (unsigned)snap.safe_reason);
        }

        for (i = 0; i < TEST_ITEM_COUNT; i++) {
            if (snap.items[i].result != s_prev.items[i].result ||
                strncmp(snap.items[i].detail, s_prev.items[i].detail, TEST_DETAIL_LEN) != 0) {
                u8 phase = (snap.items[i].result == TRES_RUNNING) ? (u8)PSU_ITEM_PHASE_START
                                                                 : (u8)PSU_ITEM_PHASE_END;
                send_item(i, phase, &snap.items[i]);
            }
        }
    }

    s_prev = snap;
    s_prev_valid = 1u;
}

/* ---------------- 看门狗 ---------------- */

static void wd_check(void)
{
    u8 d[2];

    if (s_wd_timeout_ms == 0u) {
        return;
    }
    if ((u32)(now_ms() - s_wd_last_ms) <= s_wd_timeout_ms) {
        return;
    }

    /* 主机失联：走与本地长按急停同一条通路（CE# 关断 + 双 PWM 0%，序列一并中止）。
     * 原因码随 SAFE 命令进快照，日志由 poll_snapshot 的差分统一记，
     * 这里只把窗口值留给那条记录用 */
    s_wd_fired_ms = s_wd_timeout_ms;
    (void)pcb_test_post_cmd(PCB_TEST_CMD_SAFE, (u16)TEST_SAFE_REASON_WATCHDOG);
    psu_put_u16(d, (u16)s_wd_timeout_ms);
    s_wd_timeout_ms = 0u;
    send_event(PSU_EV_WATCHDOG, d, sizeof(d));
    log_printf("[LINK] 看门狗超时：已回安全态并退出远程");
}

/* ---------------- 命令分派 ---------------- */

/* 设定值换算：mV/mA → 占空比‰。板级常量只在这里用一次，上位机不需要知道。
 * app/syscfg 校验持久化配置的 VOUT/ILIM 字段时复用，所以导出（见 psu_link.h） */
exit_code_t psu_link_vout_to_permille(u8 unit, u16 val, u16 *out)
{
    u32 num;

    if (unit == (u8)PSU_UNIT_PERMILLE) {
        if (val > 1000u) {
            return EXIT_INVALID_PARAM;
        }
        *out = val;
        return EXIT_OK;
    }
    if (unit != (u8)PSU_UNIT_MV) {
        return EXIT_INVALID_PARAM;
    }
    /* VSET(D) = PCB_VOUT_SET_MV × (1 + 5D) / 6  →  D = (6·VSET/FULL − 1) / 5 */
    if (val < (u16)(PCB_VOUT_SET_MV / 6u) || val > (u16)PCB_VOUT_SET_MV) {
        return EXIT_INVALID_PARAM;
    }
    num = 1200u * (u32)val - 200u * (u32)PCB_VOUT_SET_MV;
    *out = (u16)((num + (u32)PCB_VOUT_SET_MV / 2u) / (u32)PCB_VOUT_SET_MV);
    return EXIT_OK;
}

exit_code_t psu_link_ilim_to_permille(u8 unit, u16 val, u16 *out)
{
    if (unit == (u8)PSU_UNIT_PERMILLE) {
        if (val > 1000u) {
            return EXIT_INVALID_PARAM;
        }
        *out = val;
        return EXIT_OK;
    }
    if (unit != (u8)PSU_UNIT_MV) {      /* SET_ILIM 的 "mV 单位" 即 mA */
        return EXIT_INVALID_PARAM;
    }
    if (val > (u16)PCB_ILIM2_FULL_MA) {
        return EXIT_INVALID_PARAM;
    }
    *out = (u16)(((u32)val * 1000u + (u32)PCB_ILIM2_FULL_MA / 2u) / (u32)PCB_ILIM2_FULL_MA);
    return EXIT_OK;
}

/* SET_VOUT / SET_ILIM 的公共流程：换算 → 忙检查 → post → ACK（arg = 生效占空比） */
static void apply_setpoint(u8 seq, u8 cmd, const psu_frame_t *req)
{
    u8 unit;
    u16 val;
    u16 permille = 0;
    exit_code_t rc;

    if (req->len != 3u) {
        send_ack(seq, cmd, EXIT_INVALID_PARAM, 0);
        return;
    }
    unit = req->payload[0];
    val = psu_get_u16(&req->payload[1]);

    rc = (cmd == (u8)PSU_CMD_SET_VOUT) ? psu_link_vout_to_permille(unit, val, &permille)
                                       : psu_link_ilim_to_permille(unit, val, &permille);
    if (rc != EXIT_OK) {
        send_ack(seq, cmd, rc, 0);
        return;
    }
    if (test_busy()) {
        send_ack(seq, cmd, EXIT_BUSY, permille);
        return;
    }
    rc = pcb_test_post_cmd((cmd == (u8)PSU_CMD_SET_VOUT) ? PCB_TEST_CMD_SET_VOUT : PCB_TEST_CMD_SET_ILIM,
                           permille);
    if (rc == EXIT_OK) {
        /* 预设值跟着设定值走（去抖落盘），这样 `cfg restore` 回填的确实是上次用的值 */
        syscfg_note_setpoint((cmd == (u8)PSU_CMD_SET_VOUT) ? (u8)PSU_CFG_F_VOUT
                                                          : (u8)PSU_CFG_F_ILIM, permille);
    }
    send_ack(seq, cmd, rc, permille);
}

/* 完整状态快照：握手/重连时用，一次给出实时块 + 各项结果，随后把 7 项都发一遍
 * （含 PENDING）—— 上位机靠 ITEM 帧才知道测试项的名字，全发一遍表格才是完整的。 */
static void send_status(u8 seq)
{
    test_status_t snap;
    u8 i;

    pcb_test_get_status(&snap);
    send_telemetry(seq, &snap, true);
    for (i = 0; i < TEST_ITEM_COUNT; i++) {
        send_item(i, (u8)PSU_ITEM_PHASE_END, &snap.items[i]);
    }
}

static void handle_frame(const psu_frame_t *f)
{
    exit_code_t rc;

    /* 任何一条 CRC 通过的主机帧都算"主机还在" */
    s_wd_last_ms = now_ms();

    switch (f->cmd) {
    case PSU_CMD_PING:
        send_pong(f->seq, f);
        break;

    case PSU_CMD_GET_INFO:
        send_info(f->seq);
        break;

    case PSU_CMD_GET_STATUS:
        send_status(f->seq);
        break;

    case PSU_CMD_SET_VOUT:
    case PSU_CMD_SET_ILIM:
        apply_setpoint(f->seq, f->cmd, f);
        break;

    case PSU_CMD_SET_OUTPUT:
        if (f->len != 1u || f->payload[0] > 1u) {
            send_ack(f->seq, f->cmd, EXIT_INVALID_PARAM, 0);
        } else if (test_busy()) {
            send_ack(f->seq, f->cmd, EXIT_BUSY, 0);
        } else {
            rc = pcb_test_post_cmd(PCB_TEST_CMD_SET_OUTPUT, f->payload[0]);
            send_ack(f->seq, f->cmd, rc, f->payload[0]);
        }
        break;

    case PSU_CMD_SAFE_STATE:
        rc = pcb_test_post_cmd(PCB_TEST_CMD_SAFE, (u16)TEST_SAFE_REASON_HOST);
        send_ack(f->seq, f->cmd, rc, 0);
        break;

    case PSU_CMD_RUN_TEST:
        if (test_busy()) {
            send_ack(f->seq, f->cmd, EXIT_BUSY, 0);
        } else {
            rc = pcb_test_post_cmd(PCB_TEST_CMD_RUN, 0);
            send_ack(f->seq, f->cmd, rc, 0);
        }
        break;

    case PSU_CMD_STOP_TEST:
        rc = pcb_test_post_cmd(PCB_TEST_CMD_STOP, 0);
        send_ack(f->seq, f->cmd, rc, 0);
        break;

    case PSU_CMD_CFG_GET:
        send_cfg(f->seq);
        break;

    case PSU_CMD_CFG_SET:
        handle_cfg_set(f->seq, f);
        break;

    case PSU_CMD_CFG_RESET:
        rc = syscfg_reset();
        if (rc == EXIT_OK) {
            /* 恢复出厂后运行参数也跟着回默认值（只有遥测周期是可自动应用的） */
            s_telem_period_ms = PSU_LINK_TELEM_PERIOD_MS;
            s_telem_next_ms = now_ms() + s_telem_period_ms;
        }
        send_ack(f->seq, f->cmd, rc, 0);
        break;

    case PSU_CMD_LOG_INFO:
        send_log_info(f->seq);
        break;

    case PSU_CMD_LOG_READ:
        send_log_read(f->seq, f);
        break;

    case PSU_CMD_LOG_CLEAR:
        rc = pstore_log_clear();
        send_ack(f->seq, f->cmd, rc, 0);
        break;

    case PSU_CMD_SET_TELEM:
        if (f->len != 2u) {
            send_ack(f->seq, f->cmd, EXIT_INVALID_PARAM, 0);
            break;
        }
        {
            u16 period = psu_get_u16(f->payload);
            if (period != 0u &&
                (period < PSU_LINK_TELEM_PERIOD_MIN || period > PSU_LINK_TELEM_PERIOD_MAX)) {
                send_ack(f->seq, f->cmd, EXIT_INVALID_PARAM, period);
                break;
            }
            s_telem_period_ms = period;
            s_telem_next_ms = now_ms() + period;
            s_telem_from_host = 1u;
            send_ack(f->seq, f->cmd, EXIT_OK, period);
        }
        break;

    case PSU_CMD_SET_REMOTE:
        if (f->len != 3u) {
            send_ack(f->seq, f->cmd, EXIT_INVALID_PARAM, 0);
            break;
        }
        {
            u8 enable = f->payload[0];
            u16 timeout = psu_get_u16(&f->payload[1]);

            if (enable > 1u) {
                send_ack(f->seq, f->cmd, EXIT_INVALID_PARAM, 0);
                break;
            }
            if (enable != 0u && timeout != 0u &&
                (timeout < PSU_LINK_WD_TIMEOUT_MIN_MS || timeout > PSU_LINK_WD_TIMEOUT_MAX_MS)) {
                send_ack(f->seq, f->cmd, EXIT_INVALID_PARAM, timeout);
                break;
            }
            rc = pcb_test_post_cmd(PCB_TEST_CMD_REMOTE, enable);
            if (rc == EXIT_OK) {
                /* 远程模式的真值由 pcb_test_task 写进快照；这里只武装看门狗并给足第一个窗口 */
                s_wd_timeout_ms = (enable != 0u) ? timeout : 0u;
                s_wd_last_ms = now_ms();
                log_printf("[LINK] 远程模式 %s（看门狗 %ums）", enable ? "开启" : "关闭",
                       (unsigned)s_wd_timeout_ms);
            }
            send_ack(f->seq, f->cmd, rc, timeout);
        }
        break;

    case PSU_CMD_KEEPALIVE:
        send_ack(f->seq, f->cmd, EXIT_OK, 0);
        break;

    default:
        send_ack(f->seq, f->cmd, EXIT_NOT_SUPPORTED, 0);
        break;
    }
}

/* ---------------- 接收：逐字节分帧 ---------------- */

static void rx_reset(void)
{
    s_rx_len = 0u;
}

static void rx_block_complete(void)
{
    psu_frame_t frame;

    if (s_rx_len != 0u && psu_proto_decode(s_rx_block, s_rx_len, &frame) == EXIT_OK) {
        handle_frame(&frame);
    }
    /* 解不出来的一律丢：可能是 printf 文本混进来，也可能是被文本劈开的半帧 */
    rx_reset();
}

static void rx_byte(u8 b)
{
    if (b == 0x00) {
        rx_block_complete();
        return;
    }
    if (s_rx_len >= sizeof(s_rx_block)) {
        rx_reset();                     /* 块超长：丢到下一个分隔符为止 */
        return;
    }
    s_rx_block[s_rx_len++] = b;
}

static void rx_poll(void)
{
    u8 buf[64];
    u32 n;
    u32 i;
    u8 round;

    /* 一次最多取 4×64 字节，避免主机灌数据时把这个任务卡在收里 */
    for (round = 0; round < 4u; round++) {
        n = tud_cdc_read(buf, sizeof(buf));
        if (n == 0u) {
            break;
        }
        for (i = 0; i < n; i++) {
            rx_byte(buf[i]);
        }
    }
}

static void telemetry_check(void)
{
    if (s_telem_period_ms == 0u) {
        return;
    }
    if ((i32)(now_ms() - s_telem_next_ms) < 0) {
        return;
    }
    s_telem_next_ms = now_ms() + s_telem_period_ms;
    send_telemetry(0u, &s_prev, false);
}

/* ---------------- 对外接口 ---------------- */

/* 幂等：配置第一次就绪时把"可自动应用"的那部分套上。
 * 只套用非安全项（遥测周期）：电压/限流预设与输出开关留在安全态，
 * 由主机显式下发（CLI 的 `cfg restore`），这样上电永远是安全态。
 * 主机要是自己设过遥测周期，就不再拿默认值覆盖它。 */
static void apply_persisted_cfg(void)
{
    syscfg_t cfg;
    u8 flags;

    if (!syscfg_ensure_loaded()) {      /* 加载完成之前一直返回 false */
        return;
    }
    if (syscfg_get(&cfg, &flags) != EXIT_OK || s_telem_from_host) {
        return;
    }

    s_telem_period_ms = cfg.telem_period_ms;
    s_telem_next_ms = now_ms() + cfg.telem_period_ms;
}

exit_code_t psu_link_init(void)
{
    rx_reset();
    s_telem_period_ms = PSU_LINK_TELEM_PERIOD_MS;
    s_telem_next_ms = now_ms() + s_telem_period_ms;
    s_wd_timeout_ms = 0u;
    s_wd_last_ms = now_ms();
    s_wd_fired_ms = 0u;
    s_prev_valid = 0u;
    s_telem_from_host = 0u;
    s_reset_reported = 0u;
    memset(&s_prev, 0, sizeof(s_prev));
    return EXIT_OK;
}

void psu_link_task(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        /* 主机一连上就把上次复位的原因报出去（打印完会清掉 scratch 标记） */
        if (!s_reset_reported && tud_cdc_connected()) {
            s_reset_reported = 1u;
            reset_reason_report();
        }
        apply_persisted_cfg();  /* 配置就绪后套用一次（幂等） */
        syscfg_tick();          /* 设定值改动的去抖落盘 */
        rx_poll();              /* 收命令 */
        poll_snapshot();        /* 快照差分 → 事件/测试项上报/安全态日志 */
        wd_check();             /* 主机失联看门狗 */
        telemetry_check();      /* 周期遥测 */
        /* 收尾：把本轮产生的日志吐给主机（主机没连上时原样留着）。
         * 放在最后，启动那几条积压的日志也能在第一次循环就发出去 */
        log_drain();
        vTaskDelay(pdMS_TO_TICKS(PSU_LINK_POLL_MS));
    }
}
