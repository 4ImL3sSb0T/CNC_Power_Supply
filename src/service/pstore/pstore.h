/**
 * @file        pstore.h
 * @brief       持久化存储服务：配置 blob + 事件日志，单写者任务
 *
 * 磁盘布局（littlefs 分区 256KB 的尾部，见 src/service/fs/fs.c）：
 *
 *   /cfg/psu.cfg          配置 blob：u8 len + blob[len] + u16 crc16（本层只认这个信封，
 *                         里面的字段语义归 app/syscfg）
 *   /log/evt.NNNN         日志段，每段 PSTORE_SEG_SIZE 字节：
 *                           偏移 0..15     段头记录（type = PSTORE_REC_SEG，
 *                                          arg = 本段第一条数据记录的全局序号）
 *                           偏移 16..4095  255 条 16 字节数据记录
 *                         共 16 段 = 64KB = 4080 条，环绕覆盖最旧段
 *
 * 记录号 → 位置是纯算术：段号 = (rec_index / 255) % 16，段内偏移 = 16 + (rec_index % 255) * 16。
 * 段固定写满才轮转，所以不需要索引文件；段头里的全局序号让上电时扫一遍 16 个段头就能
 * 还原 first/next 序号（也不依赖额外的状态文件）。
 *
 * 为什么按段轮转而不是单文件环形：littlefs 不支持原地改写文件头，单文件要么 truncate(0)
 * 丢掉全部历史，要么 seek(0) 改写触发整文件 COW。分段的生命周期是"创建 → 追加 → 写满 →
 * 删除"，写放大最小。段大小取 4KB（= 一个 block = 一次 cache 填满）也是同一个理由：
 * 少一次跨核握手就少付一次握手开销（见 src/service/fs/fs.c 文件头）。
 *
 * 线程约定：
 *   - 所有 Flash **写**只在 pstore_task 里发生（单写者），其它任务经队列投递，非阻塞
 *   - 读是内联的（XIP memcpy，不写 Flash），且整体用 fs_lock() 与写者串行
 *   - 日志量本来就低（只记上电与急停），所以不做攒批 ring，一条记录直接落盘
 */

#ifndef PSTORE_H
#define PSTORE_H

#include <stdbool.h>
#include "lib/tools/common_def.h"

#define PSTORE_REC_SIZE      16u
#define PSTORE_SEG_SIZE      4096u
#define PSTORE_SEG_COUNT     16u
#define PSTORE_RECS_PER_SEG  ((PSTORE_SEG_SIZE - PSTORE_REC_SIZE) / PSTORE_REC_SIZE)   /* 255 */
#define PSTORE_REC_CAPACITY  (PSTORE_RECS_PER_SEG * PSTORE_SEG_COUNT)                  /* 4080 */
#define PSTORE_BLOB_MAX      48u        /* 配置 blob 上限，够放 32 字节结构 + 余量 */
#define PSTORE_QUEUE_LEN     16u

/* 记录类型。type 字段是 u8，1 之后留给后续扩展（测试结果、配置变更等） */
typedef enum {
    PSTORE_REC_SEG  = 0,    /* 段头，由本模块内部产生，不会出现在 pstore_log_read 的结果里 */
    PSTORE_REC_BOOT = 1,    /* a/b = 固件主/次版本，arg = 启动序号 */
    PSTORE_REC_SAFE = 2,    /* a = 回安全态的原因（test_safe_reason_t），arg = 看门狗超时值 */
} pstore_rec_type_t;

/* 16 字节定长记录（穿越 Flash 与协议，逐字段小端序列化，不做结构体直投） */
typedef struct {
    u32 tick_ms;            /* 设备运行时间 */
    u8  type;               /* pstore_rec_type_t */
    u8  a;                  /* 类型相关 */
    u8  b;                  /* 类型相关 */
    u8  rsv;                /* 补位，写 0 */
    u16 pwm_permille;       /* 事件瞬间的电压设定占空比 */
    u16 ipwm_permille;      /* 事件瞬间的限流设定占空比 */
    u32 arg;                /* 类型相关 */
} pstore_rec_t;

typedef struct {
    u8  rec_size;           /* 固定 16 */
    u8  seg_count;          /* 固定 16 */
    u16 seg_size;           /* 固定 4096 */
    u32 total;              /* 当前可读的记录条数 */
    u32 first_index;        /* 最旧一条的全局序号（环形覆盖后非 0） */
} pstore_info_t;

/* ---------------- 任务 ---------------- */

/* 常驻：挂载文件系统、载入配置、处理写队列。**绝不 vTaskDelete**
 * （它结束就意味着后续所有落盘请求石沉大海）。必须在调度器启动后创建，
 * 因为 fs_init() 写 Flash 需要跨核锁另一核（见 fs.c 文件头）。 */
void pstore_task(void *pvParameters);

/* ---------------- 配置 ---------------- */

/* 文件系统已挂载且配置已载入（挂载失败时为 false，调用方据此决定要不要宣称支持持久化） */
bool        pstore_ready(void);

/* 读配置的 RAM 镜像（无 Flash 访问）。cap 不够返回 EXIT_NO_MEMORY，
 * 没有配置文件或校验不过返回 EXIT_DOES_NOT_EXIST / EXIT_CRC_MISMATCH */
exit_code_t pstore_cfg_get(u8 *blob, u32 cap, u32 *out_len);

/* 异步保存：blob 拷进队列后立即返回，由 pstore_task 落盘并加信封（len + crc16）。
 * 队列满返回 EXIT_BUSY。配置写入本身极低频（上位机手改 + 设定值去抖），
 * 所以不做过期合并：同一次改动重复入队时按顺序各写一遍，最后一次生效。 */
exit_code_t pstore_cfg_save(const u8 *blob, u32 len);

/* 有未落盘的配置改动（上位机据此确认 CFG_SET 是否真的写进去了） */
bool        pstore_cfg_dirty(void);

/* ---------------- 日志 ---------------- */

/* 填好 tick_ms 与 type，其余清零 */
void        pstore_rec_init(pstore_rec_t *rec, u8 type);

/* 把记录打成 16 字节上线格式。落盘与协议 payload 共用这一份实现（导出给 psu_link） */
void        pstore_rec_pack(const pstore_rec_t *rec, u8 *out);

/* 非阻塞投递（任意任务上下文）。队列满返回 EXIT_BUSY —— 调用方可以丢弃，
 * 因为日志量极低，队列满本身说明写者卡住了（大概率是文件系统故障） */
exit_code_t pstore_log_append(const pstore_rec_t *rec);

/* 段/容量/序号信息（内联读，加锁） */
exit_code_t pstore_log_info(pstore_info_t *out);

/* 从全局记录号 rec_index 起读最多 nrec 条（nrec ≤ 2，受 48 字节 payload 限制）。
 * 读不到（越界或文件缺失）时 *out_nrec = 0 并返回 EXIT_OK，调用方据此结束循环 */
exit_code_t pstore_log_read(u32 rec_index, u8 nrec, pstore_rec_t *out, u8 *out_nrec);

/* 异步清空全部日志段，记录号从 0 重新开始 */
exit_code_t pstore_log_clear(void);

#endif /* PSTORE_H */
