/**
 * @file        pstore.c
 * @brief       持久化存储服务实现：单写者任务 + 段式日志 + 配置 blob
 *
 * 读写分工（细节见 pstore.h 文件头的线程约定）：
 *   - 写：只有 pstore_task 调 lfs_file_open(写) / lfs_remove，请求经队列进来
 *   - 读：调用方上下文内联完成（XIP memcpy，不写 Flash），整段包在 fs_lock() 里
 * 复合序列（open→write→close、删旧段+建新段）必须整体加锁，否则会被另一任务的
 * open/close 穿插。
 */

#include "service/pstore/pstore.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "pico/time.h"
#include "service/fs/fs.h"
#include "lib/proto/psu_proto.h"        /* psu_crc16 / psu_put_* / psu_get_* —— 纯工具，不涉及链路 */

#define PSTORE_CFG_DIR   "/cfg"
#define PSTORE_CFG_PATH  "/cfg/psu.cfg"
#define PSTORE_LOG_DIR   "/log"

/* 配置文件的信封：u8 len | blob[len] | u16 crc16（crc 覆盖 len + blob） */
#define PSTORE_CFG_ENVELOPE   3u
#define PSTORE_CFG_FILE_MAX   (1u + PSTORE_BLOB_MAX + 2u)

typedef enum {
    PSTORE_MSG_REC = 1,         /* 追加一条日志记录 */
    PSTORE_MSG_CFG = 2,         /* 落盘配置 blob */
    PSTORE_MSG_CLEAR = 3,       /* 清空全部日志段 */
} pstore_msg_kind_t;

typedef struct {
    u8 kind;
    u8 len;                     /* PSTORE_MSG_CFG 时有效 */
    pstore_rec_t rec;
    u8 blob[PSTORE_BLOB_MAX];
} pstore_msg_t;

static lfs_t *s_lfs;
static bool   s_ready;
static QueueHandle_t s_queue;

static u32 s_first_index;       /* 最旧一条的全局序号 */
static u32 s_next_index;        /* 下一条要写的全局序号 */

static u8   s_cfg_blob[PSTORE_BLOB_MAX];
static u32  s_cfg_len;
static bool s_cfg_loaded;       /* 镜像来自一次成功的文件读取 */
static volatile bool s_cfg_dirty;   /* 有已入队但还没落盘的配置改动 */

/* ---------------- 序列化 ---------------- */

/* 落盘格式与协议 payload 是同一套 16 字节布局，所以只留这一份打包实现（见 pstore.h） */
void pstore_rec_pack(const pstore_rec_t *r, u8 *b)
{
    psu_put_u32(&b[0], r->tick_ms);
    b[4] = r->type;
    b[5] = r->a;
    b[6] = r->b;
    b[7] = r->rsv;
    psu_put_u16(&b[8], r->pwm_permille);
    psu_put_u16(&b[10], r->ipwm_permille);
    psu_put_u32(&b[12], r->arg);
}

static void rec_unpack(const u8 *b, pstore_rec_t *r)
{
    r->tick_ms = psu_get_u32(&b[0]);
    r->type = b[4];
    r->a = b[5];
    r->b = b[6];
    r->rsv = b[7];
    r->pwm_permille = psu_get_u16(&b[8]);
    r->ipwm_permille = psu_get_u16(&b[10]);
    r->arg = psu_get_u32(&b[12]);
}

void pstore_rec_init(pstore_rec_t *rec, u8 type)
{
    memset(rec, 0, sizeof(*rec));
    rec->tick_ms = (u32)to_ms_since_boot(get_absolute_time());
    rec->type = type;
}

/* ---------------- 段路径 ---------------- */

static void seg_path(char *out, size_t n, u32 seg)
{
    snprintf(out, n, PSTORE_LOG_DIR "/evt.%04u", (unsigned)seg);
}

/* ---------------- 配置（只在写者任务里跑） ---------------- */

static void cfg_read_sync(void)
{
    u8 buf[PSTORE_CFG_FILE_MAX];
    lfs_file_t f;
    lfs_ssize_t n;
    u32 len;

    s_cfg_loaded = false;
    s_cfg_len = 0u;
    s_cfg_dirty = false;

    fs_lock();
    if (lfs_file_open(s_lfs, &f, PSTORE_CFG_PATH, LFS_O_RDONLY) != LFS_ERR_OK) {
        fs_unlock();
        return;                             /* 还没有配置文件：调用方用默认值 */
    }
    n = lfs_file_read(s_lfs, &f, buf, sizeof(buf));
    (void)lfs_file_close(s_lfs, &f);
    fs_unlock();

    if (n < (lfs_ssize_t)PSTORE_CFG_ENVELOPE) {
        return;
    }
    len = buf[0];
    if ((lfs_ssize_t)(1u + len + 2u) != n || len > PSTORE_BLOB_MAX) {
        return;                             /* 长度对不上：当作损坏，保留文件当证据 */
    }
    if (psu_crc16(buf, 1u + len) != psu_get_u16(&buf[1u + len])) {
        return;                             /* CRC 不过：同上 */
    }

    memcpy(s_cfg_blob, &buf[1], len);
    s_cfg_len = len;
    s_cfg_loaded = true;
}

static exit_code_t cfg_write_sync(const u8 *blob, u32 len)
{
    u8 buf[PSTORE_CFG_FILE_MAX];
    lfs_file_t f;
    int err;

    if (len > PSTORE_BLOB_MAX) {
        return EXIT_INVALID_PARAM;
    }
    buf[0] = (u8)len;
    memcpy(&buf[1], blob, len);
    psu_put_u16(&buf[1u + len], psu_crc16(buf, 1u + len));

    fs_lock();
    err = lfs_file_open(s_lfs, &f, PSTORE_CFG_PATH,
                        LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err == LFS_ERR_OK) {
        if (lfs_file_write(s_lfs, &f, buf, 1u + len + 2u) != (lfs_ssize_t)(1u + len + 2u)) {
            err = LFS_ERR_IO;
        }
        if (lfs_file_close(s_lfs, &f) != LFS_ERR_OK) {
            err = LFS_ERR_IO;
        }
    }
    fs_unlock();

    return (err == LFS_ERR_OK) ? EXIT_OK : EXIT_FAIL;
}

/* ---------------- 日志（写：只在写者任务里跑） ---------------- */

/* 扫 16 个段头还原 first/next 序号：段头里的 arg 是本段首条记录的全局序号，
 * 段按序号递增创建，所以"最大的 arg"就是活动段 */
static void log_scan(void)
{
    u32 first = 0u;
    u32 active_first = 0u;
    u32 active_count = 0u;
    bool any = false;
    u32 i;

    s_first_index = 0u;
    s_next_index = 0u;

    for (i = 0u; i < PSTORE_SEG_COUNT; i++) {
        char path[32];
        lfs_file_t f;
        u8 hdr[PSTORE_REC_SIZE];
        pstore_rec_t h;
        lfs_ssize_t n;
        lfs_ssize_t size;
        u32 count;

        seg_path(path, sizeof(path), i);

        fs_lock();
        if (lfs_file_open(s_lfs, &f, path, LFS_O_RDONLY) != LFS_ERR_OK) {
            fs_unlock();
            continue;
        }
        n = lfs_file_read(s_lfs, &f, hdr, sizeof(hdr));
        size = lfs_file_size(s_lfs, &f);
        (void)lfs_file_close(s_lfs, &f);
        fs_unlock();

        if (n < (lfs_ssize_t)sizeof(hdr) || size < (lfs_ssize_t)PSTORE_REC_SIZE) {
            continue;                       /* 空段（建档后掉电） */
        }
        rec_unpack(hdr, &h);
        if (h.type != (u8)PSTORE_REC_SEG) {
            continue;                       /* 段头不合法，整段不认 */
        }

        count = (u32)(size - (lfs_ssize_t)PSTORE_REC_SIZE) / PSTORE_REC_SIZE;

        if (!any || h.arg < first) {
            first = h.arg;
        }
        if (!any || h.arg > active_first) {
            active_first = h.arg;
            active_count = count;
        }
        any = true;
    }

    if (any) {
        s_first_index = first;
        s_next_index = active_first + active_count;
    }
}

static exit_code_t log_append_sync(const pstore_rec_t *rec)
{
    u32 idx = s_next_index;
    u32 seg = (idx / PSTORE_RECS_PER_SEG) % PSTORE_SEG_COUNT;
    char path[32];
    u8 buf[PSTORE_REC_SIZE];
    lfs_file_t f;
    int err = LFS_ERR_OK;

    fs_lock();

    if ((idx % PSTORE_RECS_PER_SEG) == 0u) {
        /* 新段。段号是环绕的，先删掉同槽位那一代（它是最旧的），再建带段头的新段 */
        pstore_rec_t hdr;

        seg_path(path, sizeof(path), seg);
        (void)lfs_remove(s_lfs, path);

        pstore_rec_init(&hdr, (u8)PSTORE_REC_SEG);
        hdr.arg = idx;
        pstore_rec_pack(&hdr, buf);

        err = lfs_file_open(s_lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
        if (err == LFS_ERR_OK) {
            if (lfs_file_write(s_lfs, &f, buf, sizeof(buf)) != (lfs_ssize_t)sizeof(buf)) {
                err = LFS_ERR_IO;
            }
            if (lfs_file_close(s_lfs, &f) != LFS_ERR_OK) {
                err = LFS_ERR_IO;
            }
        }
    }

    if (err == LFS_ERR_OK) {
        seg_path(path, sizeof(path), seg);
        pstore_rec_pack(rec, buf);

        err = lfs_file_open(s_lfs, &f, path, LFS_O_WRONLY | LFS_O_APPEND);
        if (err == LFS_ERR_OK) {
            if (lfs_file_write(s_lfs, &f, buf, sizeof(buf)) != (lfs_ssize_t)sizeof(buf)) {
                err = LFS_ERR_IO;
            }
            if (lfs_file_close(s_lfs, &f) != LFS_ERR_OK) {
                err = LFS_ERR_IO;
            }
        }
    }

    fs_unlock();

    if (err != LFS_ERR_OK) {
        return EXIT_FAIL;
    }
    s_next_index++;                         /* 失败就不推进：下一条重试同一槽位 */
    return EXIT_OK;
}

static void log_clear_sync(void)
{
    u32 i;

    fs_lock();
    for (i = 0u; i < PSTORE_SEG_COUNT; i++) {
        char path[32];
        seg_path(path, sizeof(path), i);
        (void)lfs_remove(s_lfs, path);      /* 不存在也无所谓 */
    }
    fs_unlock();

    s_first_index = 0u;
    s_next_index = 0u;
}

/* ---------------- 任务 ---------------- */

static void init_fs(void)
{
    int err;

    /* 队列先建：挂载期间来的落盘请求能排着，挂载完成后一起处理 */
    s_queue = xQueueCreate(PSTORE_QUEUE_LEN, sizeof(pstore_msg_t));

    err = fs_init();
    if (err != LFS_ERR_OK) {
        printf("[PSTORE] 文件系统不可用（%d），持久化功能关闭\n", err);
        return;
    }
    s_lfs = fs_get_handle();

    fs_lock();
    (void)lfs_mkdir(s_lfs, PSTORE_CFG_DIR);
    (void)lfs_mkdir(s_lfs, PSTORE_LOG_DIR);
    fs_unlock();

    cfg_read_sync();
    log_scan();
    s_ready = true;

    printf("[PSTORE] 就绪：配置%s，日志 %u 条\n",
           s_cfg_loaded ? "已载入" : "无（用默认值）",
           (unsigned)(s_next_index - s_first_index));
}

void pstore_task(void *pvParameters)
{
    (void)pvParameters;

    init_fs();

    for (;;) {
        pstore_msg_t msg;

        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_ready) {
            continue;                       /* 挂载失败：丢弃请求，不假装成功 */
        }

        switch ((pstore_msg_kind_t)msg.kind) {
        case PSTORE_MSG_REC:
            (void)log_append_sync(&msg.rec);
            break;

        case PSTORE_MSG_CFG:
            if (cfg_write_sync(msg.blob, msg.len) == EXIT_OK) {
                s_cfg_dirty = false;
            }
            break;

        case PSTORE_MSG_CLEAR:
            log_clear_sync();
            break;

        default:
            break;
        }
    }
}

/* ---------------- 对外接口 ---------------- */

bool pstore_ready(void)
{
    return s_ready;
}

exit_code_t pstore_cfg_get(u8 *blob, u32 cap, u32 *out_len)
{
    if (!s_ready) {
        return EXIT_NOT_INITIALIZED;
    }
    if (!s_cfg_loaded) {
        return EXIT_DOES_NOT_EXIST;         /* 无文件或校验不过，调用方用默认值 */
    }
    if (cap < s_cfg_len) {
        return EXIT_NO_MEMORY;
    }
    memcpy(blob, s_cfg_blob, s_cfg_len);
    *out_len = s_cfg_len;
    return EXIT_OK;
}

exit_code_t pstore_cfg_save(const u8 *blob, u32 len)
{
    pstore_msg_t msg;

    if (s_queue == NULL) {
        return EXIT_NOT_INITIALIZED;
    }
    if (len > PSTORE_BLOB_MAX) {
        return EXIT_INVALID_PARAM;
    }

    memset(&msg, 0, sizeof(msg));
    msg.kind = (u8)PSTORE_MSG_CFG;
    msg.len = (u8)len;
    memcpy(msg.blob, blob, len);

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        return EXIT_BUSY;                   /* 队列满：调用方可以稍后重试 */
    }
    s_cfg_dirty = true;                     /* 先置位：落盘成功才由写者清掉 */
    return EXIT_OK;
}

bool pstore_cfg_dirty(void)
{
    return s_cfg_dirty;
}

exit_code_t pstore_log_append(const pstore_rec_t *rec)
{
    pstore_msg_t msg;

    if (s_queue == NULL) {
        return EXIT_NOT_INITIALIZED;
    }

    memset(&msg, 0, sizeof(msg));
    msg.kind = (u8)PSTORE_MSG_REC;
    msg.rec = *rec;

    return (xQueueSend(s_queue, &msg, 0) == pdTRUE) ? EXIT_OK : EXIT_BUSY;
}

exit_code_t pstore_log_info(pstore_info_t *out)
{
    if (!s_ready) {
        return EXIT_NOT_INITIALIZED;
    }

    out->rec_size = (u8)PSTORE_REC_SIZE;
    out->seg_count = (u8)PSTORE_SEG_COUNT;
    out->seg_size = (u16)PSTORE_SEG_SIZE;
    out->total = s_next_index - s_first_index;
    out->first_index = s_first_index;
    return EXIT_OK;
}

exit_code_t pstore_log_read(u32 rec_index, u8 nrec, pstore_rec_t *out, u8 *out_nrec)
{
    u8 got = 0u;
    u8 i;

    *out_nrec = 0u;
    if (!s_ready) {
        return EXIT_NOT_INITIALIZED;
    }
    if (nrec > 2u) {
        nrec = 2u;                          /* 48 字节 payload 装不下第 3 条 */
    }

    fs_lock();
    for (i = 0u; i < nrec; i++) {
        u32 idx = rec_index + i;
        u32 seg;
        u32 off;
        char path[32];
        u8 buf[PSTORE_REC_SIZE];
        lfs_file_t f;
        lfs_ssize_t n;

        if (idx < s_first_index || idx >= s_next_index) {
            break;                          /* 越界或已被环形覆盖：到此为止 */
        }
        seg = (idx / PSTORE_RECS_PER_SEG) % PSTORE_SEG_COUNT;
        off = PSTORE_REC_SIZE + (idx % PSTORE_RECS_PER_SEG) * PSTORE_REC_SIZE;

        seg_path(path, sizeof(path), seg);
        if (lfs_file_open(s_lfs, &f, path, LFS_O_RDONLY) != LFS_ERR_OK) {
            break;
        }
        if (lfs_file_seek(s_lfs, &f, (lfs_soff_t)off, LFS_SEEK_SET) < 0) {
            (void)lfs_file_close(s_lfs, &f);
            break;
        }
        n = lfs_file_read(s_lfs, &f, buf, sizeof(buf));
        (void)lfs_file_close(s_lfs, &f);
        if (n != (lfs_ssize_t)sizeof(buf)) {
            break;
        }
        rec_unpack(buf, &out[got]);
        got++;
    }
    fs_unlock();

    *out_nrec = got;
    return EXIT_OK;
}

exit_code_t pstore_log_clear(void)
{
    pstore_msg_t msg;

    if (s_queue == NULL) {
        return EXIT_NOT_INITIALIZED;
    }

    memset(&msg, 0, sizeof(msg));
    msg.kind = (u8)PSTORE_MSG_CLEAR;

    return (xQueueSend(s_queue, &msg, 0) == pdTRUE) ? EXIT_OK : EXIT_BUSY;
}
