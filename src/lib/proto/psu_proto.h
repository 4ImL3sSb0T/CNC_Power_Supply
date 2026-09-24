/**
 * @file        psu_proto.h
 * @brief       上位机 ↔ 测试台 通信协议：帧格式、COBS/CRC 编解码、命令与事件定义
 *
 * 物理链路：USB CDC（TinyUSB 原生读写，见 app/psu_link/psu_link.c）。
 * 链路上的一帧 = 0x00 + COBS(帧内容) + 0x00（前后各一个分隔符，接收端跳过空块），
 * 帧内容（小端）：
 *
 *   +--------+--------+--------+------------------+------------------+
 *   |  cmd   |  seq   |  len   |     payload      |      crc16       |
 *   | 1 字节 | 1 字节 | 1 字节 |     len 字节     |      2 字节      |
 *   +--------+--------+--------+------------------+------------------+
 *            |<------- CRC-16/CCITT-FALSE 覆盖范围 --------->|
 *
 * cmd 的 bit7 表示方向：0 = 上位机→设备（命令），1 = 设备→上位机（响应/上报）。
 * seq 用于请求-响应配对：设备在响应里原样回填；设备主动上报（遥测/事件）填 0。
 *
 * 前置分隔符的用途：printf 日志文本也走这条 CDC，而文本里不会出现 0x00。少一个前置
 * 分隔符的话，"一行日志 + 紧跟其后的帧"会粘成同一个块，整块 CRC 失败会连带把那一帧
 * 丢掉；有了前置分隔符，日志在帧之前就被切成独立的文本块，帧本身完好无损。
 *
 * 帧长上限：payload 最多 PSU_PROTO_MAX_PAYLOAD 字节，帧内容最长 53 字节，COBS 编码后
 * 最长 54 字节，前后分隔符各 1 字节 → 链路上一帧最长 56 字节 —— 恰好能一次
 * tud_cdc_write 塞进 64 字节的 CDC TX FIFO，不会被别的任务中途插进来的 printf 劈成两半。
 *
 * 为什么用 COBS 分帧：编码结果里不会出现 0x00。所以日志文本混进协议流时，错位的块
 * 只会在 CRC 上失败被丢弃，不会被解析成一条语义错误的帧，重同步是自动的。
 */

#ifndef PSU_PROTO_H
#define PSU_PROTO_H

#include "lib/tools/common_def.h"

/* ---------------------------------------------------------------- */
/* 帧格式常量                                                        */
/* ---------------------------------------------------------------- */
#define PSU_PROTO_VERSION       1u      /* 协议版本，握手时双方核对 */

#define PSU_PROTO_MAX_PAYLOAD   48u     /* payload 上限（保证整帧 ≤ 55 字节） */
#define PSU_PROTO_MAX_BODY      (3u + PSU_PROTO_MAX_PAYLOAD + 2u)   /* 帧内容最长 53 */
#define PSU_PROTO_MAX_BLOCK     (PSU_PROTO_MAX_BODY + 1u)           /* COBS 最坏开销 1 字节 */
#define PSU_PROTO_MAX_ENCODED   (PSU_PROTO_MAX_BLOCK + 1u)          /* 再加 0x00 分隔符 */
#define PSU_PROTO_MIN_BODY      5u      /* cmd+seq+len+crc16，无 payload */

/* ---------------------------------------------------------------- */
/* 命令字（上位机 → 设备，bit7 = 0）                                 */
/* ---------------------------------------------------------------- */
typedef enum {
    PSU_CMD_PING        = 0x01,     /* payload: u32 nonce        → PONG */
    PSU_CMD_GET_INFO    = 0x02,     /* 无                        → INFO */
    PSU_CMD_SET_VOUT    = 0x03,     /* u8 unit, u16 val          → ACK(arg=生效占空比‰) */
    PSU_CMD_SET_ILIM    = 0x04,     /* u8 unit, u16 val          → ACK(arg=生效占空比‰) */
    PSU_CMD_SET_OUTPUT  = 0x05,     /* u8 on(0/1)                → ACK */
    PSU_CMD_SAFE_STATE  = 0x06,     /* 无（急停，永远接受）      → ACK */
    PSU_CMD_RUN_TEST    = 0x07,     /* 无                        → ACK */
    PSU_CMD_STOP_TEST   = 0x08,     /* 无                        → ACK */
    PSU_CMD_SET_TELEM   = 0x09,     /* u16 period_ms(0=停)       → ACK */
    PSU_CMD_SET_REMOTE  = 0x0A,     /* u8 enable, u16 timeout_ms → ACK */
    PSU_CMD_GET_STATUS  = 0x0B,     /* 无                        → STATUS + 若干 ITEM */
    PSU_CMD_KEEPALIVE   = 0x0C,     /* u32 host_ms               → ACK */
} psu_cmd_t;

/* ---------------------------------------------------------------- */
/* 响应/上报字（设备 → 上位机，bit7 = 1）                            */
/* ---------------------------------------------------------------- */
typedef enum {
    PSU_RSP_PONG    = 0x81,         /* u32 nonce, u8 proto_ver, u8 role, u16 reserved */
    PSU_RSP_INFO    = 0x82,         /* 见 psu_info_payload_t，16 字节 */
    PSU_RSP_TELEM   = 0x83,         /* 见 psu_telemetry_t，16 字节，周期上报 */
    PSU_RSP_ACK     = 0x84,         /* u8 acked_cmd, i8 code, u16 arg */
    PSU_RSP_EVENT   = 0x85,         /* u8 event, 事件相关数据 */
    PSU_RSP_ITEM    = 0x86,         /* 测试项状态变化，42 字节 */
    PSU_RSP_STATUS  = 0x8B,         /* TELEM 16 字节 + u8 item_results[7] + u8 reserved = 24 */
} psu_rsp_t;

/* SET_VOUT / SET_ILIM 的取值单位 */
typedef enum {
    PSU_UNIT_PERMILLE = 0,          /* 原始占空比 0–1000‰（精确扫描用） */
    PSU_UNIT_MV       = 1,          /* SET_VOUT 用 mV，SET_ILIM 用 mA（按人的思维设定） */
} psu_unit_t;

/* 设备信息（PSU_RSP_INFO 的 payload） */
typedef struct {
    u8  proto_ver;                  /* 协议版本 */
    u8  fw_major;                   /* 固件版本，与 CMakeLists 的 pico_set_program_version 对应 */
    u8  fw_minor;
    u8  item_count;                 /* 测试序列项数（TEST_ITEM_COUNT） */
    u16 vout_full_mv;               /* 占空比 100% 时的电压设定 */
    u16 vout_min_mv;                /* 占空比 0% 时的电压设定 */
    u16 ilim_full_ma;               /* 占空比 100% 时的限流设定 */
    u16 iin_lim_ma;                 /* 固定输入限流（不可调，仅显示） */
    u16 pwm_freq_khz;               /* SC8701 PWM 频率 */
    u8  caps;                       /* PSU_CAP_* 位 */
    u8  reserved;
} psu_info_t;

/* INFO.caps 位定义 */
#define PSU_CAP_WATCHDOG        0x01u   /* 支持主机失联看门狗 */
#define PSU_CAP_ITEM_EVENTS     0x02u   /* 会上报 ITEM 帧 */
#define PSU_CAP_VOUT_SENSE      0x04u   /* 实测 VOUT 通道已启用（板上有分压） */
#define PSU_CAP_REMOTE          0x08u   /* 支持远程模式 */
#define PSU_CAP_TELEM_PERIOD    0x10u   /* 支持配置遥测周期 */

/* 周期遥测（PSU_RSP_TELEM 的 payload，也是 PSU_RSP_STATUS 的前 16 字节） */
typedef struct {
    u8  flags;                      /* PSU_TELEM_FLAG_* 位 */
    u8  run_state;                  /* test_run_state_t */
    u8  cur_index;                  /* 当前测试项序号 */
    u8  pg_state;                   /* pcb_pg_state_t */
    u16 pwm_permille;               /* 输出电压设定占空比 */
    u16 ipwm_permille;              /* 电流限值设定占空比 */
    u16 pg_mv;                      /* PG 节点电压 */
    u16 vout_mv;                    /* 实测 VOUT（未启用时为 0） */
    u32 tick_ms;                    /* 设备运行时间 */
} psu_telemetry_t;

/* TELEM.flags 位定义 */
#define PSU_TELEM_FLAG_CE_ON        0x01u   /* CE# 已使能（输出开） */
#define PSU_TELEM_FLAG_MANUAL       0x02u   /* 手动步进模式 */
#define PSU_TELEM_FLAG_REMOTE       0x04u   /* 远程模式（按键只保留急停） */
#define PSU_TELEM_FLAG_WD_ARMED     0x08u   /* 看门狗已武装 */
#define PSU_TELEM_FLAG_VOUT_VALID   0x10u   /* vout_mv 有效 */

/* 设备→上位机事件（PSU_RSP_EVENT 的 payload：u8 event + 事件相关数据） */
typedef enum {
    PSU_EV_TEST_START = 0x01,       /* 数据：u8 item_count */
    PSU_EV_TEST_END   = 0x02,       /* 数据：u8 run_state（2=完成 3=中止） */
    PSU_EV_REMOTE     = 0x03,       /* 数据：u8 enable */
    PSU_EV_WATCHDOG   = 0x04,       /* 数据：u16 timeout_ms（看门狗超时回了安全态） */
} psu_event_t;

/* 测试项帧（PSU_RSP_ITEM 的 payload，定长 42 字节，字符串以 0 填充） */
#define PSU_ITEM_NAME_LEN       12u     /* 含结尾 0 */
#define PSU_ITEM_DETAIL_LEN     22u     /* 含结尾 0，与 test_item_t.detail 同宽 */
#define PSU_ITEM_PAYLOAD_LEN    (4u + 4u + PSU_ITEM_NAME_LEN + PSU_ITEM_DETAIL_LEN)

typedef enum {
    PSU_ITEM_PHASE_START = 1,       /* 开始执行某项 */
    PSU_ITEM_PHASE_END   = 2,       /* 该项出结果 */
} psu_item_phase_t;

/* 解出来的帧 */
typedef struct {
    u8 cmd;
    u8 seq;
    u8 len;
    u8 payload[PSU_PROTO_MAX_PAYLOAD];
} psu_frame_t;

/* ---------------------------------------------------------------- */
/* 编解码                                                            */
/* ---------------------------------------------------------------- */

/* CRC-16/CCITT-FALSE：poly 0x1021，初值 0xFFFF，不反射、不异或输出 */
u16 psu_crc16(const u8 *data, u32 len);

/* COBS 编码（不含分隔符）。返回写入 out 的字节数，负数见 exit_code_t */
i32 psu_cobs_encode(const u8 *in, u32 len, u8 *out, u32 out_size);

/* COBS 解码：in 是一段不含分隔符的 0x00 分隔块。失败返回
 * EXIT_CRC_MISMATCH 之外的具体原因：EXIT_INVALID_PARAM（块内有 0x00 / 结构不合法）、
 * EXIT_NO_MEMORY（out 装不下） */
exit_code_t psu_cobs_decode(const u8 *in, u32 len, u8 *out, u32 out_size, u32 *out_len);

/* 组帧：把 cmd/seq/payload 编成 COBS + 0x00 分隔符写进 out。
 * 返回写入总长度（含分隔符），负数见 exit_code_t（EXIT_INVALID_PARAM 参数越界、
 * EXIT_NO_MEMORY 缓冲不够） */
i32 psu_proto_encode(u8 cmd, u8 seq, const u8 *payload, u8 payload_len, u8 *out, u32 out_size);

/* 解帧：block 是以 0x00 分隔出来的一段（不含分隔符）。
 * 校验长度字段与 CRC，任一处不符返回 EXIT_INVALID_PARAM / EXIT_CRC_MISMATCH */
exit_code_t psu_proto_decode(const u8 *block, u32 block_len, psu_frame_t *frame);

/* 小端读写（协议固定小端，不依赖 MCU 字节序） */
void psu_put_u16(u8 *p, u16 v);
u16  psu_get_u16(const u8 *p);
void psu_put_u32(u8 *p, u32 v);
u32  psu_get_u32(const u8 *p);

#endif /* PSU_PROTO_H */
