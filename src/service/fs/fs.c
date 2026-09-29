/**
 * @file        fs.c
 * @brief       LittleFS 挂在板载 W25Q32 尾部 256KB；擦写走 QMI，不用 SPI
 *
 * 从 Hello_Pico 移植（裸机 → FreeRTOS SMP）改动的三处：
 *
 * 1. 线程安全：LFS_THREADSAFE（CMakeLists.txt 里的编译宏）让每个 lfs_* 公开 API
 *    进来就调 cfg.lock，配一把递归互斥锁。锁在 littlefs 内部，绕不过去——
 *    不存在"直接调 lfs_file_open 就不受锁保护"的旁路。多次 API 要当一次原子
 *    操作用 fs_lock()/fs_unlock()。
 *
 * 2. fs_init() 必须等调度器起来后在任务里调。写 Flash 走 flash_safe_execute，
 *    本工程它走 SDK 的 FreeRTOS SMP 分支：在**另一核上现场建**一个最高优先级
 *    任务去关中断。调度器没起来那个任务永远不会被调度，握手必然超时。
 *
 * 3. 跨核握手超时从 UINT32_MAX 改成有限值：本工程是真并发（两个核都跑任务），
 *    对端核可能因为优先级反转等原因此刻不让出，有限超时能让错误以 LFS_ERR_IO
 *    的形式返回给调用方，而不是原地等 49 天。
 *
 * 缓存比裸机版大 16 倍（256 B → 4096 B）：每次 prog/erase 都要付一次跨核握手
 * （建任务 + 双向 __wfe 等对端核关中断），粒度太细时握手开销比 Flash 本身还大。
 * 缓存撑到一个扇区后，元数据提交从 16 次 256 B 页写变成 1 次 4 KB 写。
 */

#include "fs.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "hardware/flash.h"
#include "pico/flash.h"

#define FS_SIZE     (256 * 1024)
#define FS_OFFSET   (PICO_FLASH_SIZE_BYTES - FS_SIZE)

/* 一次跨核握手的上限，非写入耗时上限（一个扇区擦除约 45 ms） */
#define FS_FLASH_TIMEOUT_MS  1000

/* 只能取 FLASH_PAGE_SIZE 的整数倍，且要整除 FLASH_SECTOR_SIZE（littlefs 的断言）。
 * 取到与扇区等大：读缓存、写缓存各 4 KB。 */
#define FS_CACHE_SIZE  FLASH_SECTOR_SIZE
#define FS_LOOKAHEAD_SIZE  16   /* 位图，1 字节记 8 个块；64 块只需 8 字节 */

extern char __flash_binary_end;

static uint8_t lfs_read_buf[FS_CACHE_SIZE];
static uint8_t lfs_prog_buf[FS_CACHE_SIZE];
static uint8_t lfs_lookahead_buf[FS_LOOKAHEAD_SIZE];

static SemaphoreHandle_t fs_mutex;

static int pico_read(const struct lfs_config *c, lfs_block_t block,
                     lfs_off_t off, void *buffer, lfs_size_t size)
{
    memcpy(buffer, (const uint8_t *)(XIP_BASE + FS_OFFSET)
           + (size_t)block * c->block_size + off, size);
    return LFS_ERR_OK;
}

struct flash_job {
    uint32_t offs;
    const uint8_t *data;
    size_t count;
};

static void do_erase(void *p)
{
    struct flash_job *j = p;
    flash_range_erase(j->offs, j->count);
}

static void do_program(void *p)
{
    struct flash_job *j = p;
    flash_range_program(j->offs, j->data, j->count);
}

static int pico_erase(const struct lfs_config *c, lfs_block_t block)
{
    struct flash_job j = {
        .offs  = FS_OFFSET + (uint32_t)block * c->block_size,
        .count = c->block_size,
    };
    return flash_safe_execute(do_erase, &j, FS_FLASH_TIMEOUT_MS) == PICO_OK
           ? LFS_ERR_OK : LFS_ERR_IO;
}

static int pico_prog(const struct lfs_config *c, lfs_block_t block,
                     lfs_off_t off, const void *buffer, lfs_size_t size)
{
    struct flash_job j = {
        .offs  = FS_OFFSET + (uint32_t)block * c->block_size + off,
        .data  = buffer,
        .count = size,
    };
    return flash_safe_execute(do_program, &j, FS_FLASH_TIMEOUT_MS) == PICO_OK
           ? LFS_ERR_OK : LFS_ERR_IO;
}

static int pico_sync(const struct lfs_config *c)
{
    (void)c;
    return LFS_ERR_OK;
}

static int fs_lock_cb(const struct lfs_config *c)
{
    (void)c;
    return xSemaphoreTakeRecursive(fs_mutex, portMAX_DELAY) == pdTRUE
           ? LFS_ERR_OK : LFS_ERR_IO;
}

static int fs_unlock_cb(const struct lfs_config *c)
{
    (void)c;
    return xSemaphoreGiveRecursive(fs_mutex) == pdTRUE ? LFS_ERR_OK : LFS_ERR_IO;
}

static const struct lfs_config pico_lfs_cfg = {
    .read  = pico_read,
    .prog  = pico_prog,
    .erase = pico_erase,
    .sync  = pico_sync,
    .lock  = fs_lock_cb,
    .unlock = fs_unlock_cb,
    .read_size = 1,
    .prog_size = FLASH_PAGE_SIZE,
    .block_size = FLASH_SECTOR_SIZE,
    .block_count = FS_SIZE / FLASH_SECTOR_SIZE,
    .cache_size = FS_CACHE_SIZE,
    .lookahead_size = FS_LOOKAHEAD_SIZE,
    .block_cycles = 500,
    .read_buffer = lfs_read_buf,
    .prog_buffer = lfs_prog_buf,
    .lookahead_buffer = lfs_lookahead_buf,
};

static lfs_t lfs;
static bool fs_ready;

int fs_lock(void)
{
    return fs_mutex ? fs_lock_cb(NULL) : LFS_ERR_IO;
}

int fs_unlock(void)
{
    return fs_mutex ? fs_unlock_cb(NULL) : LFS_ERR_IO;
}

int fs_init(void)
{
    uint32_t fw_end = (uint32_t)&__flash_binary_end - XIP_BASE;
    int err;

    /* 调度器没起来就不能让 lfs 走写路径（见文件头第 2 点），直接挡掉 */
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return LFS_ERR_IO;
    }

    /* 重复调用不能再建一把锁：旧锁若正被别的任务持有，会被换掉且无人释放 */
    if (fs_ready) {
        return LFS_ERR_OK;
    }

    /* 分区在 Flash 尾巴，固件不能长进 FS_OFFSET */
    if (fw_end > FS_OFFSET) {
        return LFS_ERR_IO;
    }

    /* 锁要先于 lfs_mount 存在：mount 自己就会取锁 */
    fs_mutex = xSemaphoreCreateRecursiveMutex();
    if (!fs_mutex) {
        return LFS_ERR_NOMEM;
    }

    err = lfs_mount(&lfs, &pico_lfs_cfg);
    if (err) {
        /* 无超块时 format 会擦完全部分区；已有数据 mount 失败也会被清掉 */
        err = lfs_format(&lfs, &pico_lfs_cfg);
        if (err) {
            return err;
        }
        err = lfs_mount(&lfs, &pico_lfs_cfg);
    }
    fs_ready = (err == LFS_ERR_OK);
    return err;
}

lfs_t *fs_get_handle(void)
{
    if (!fs_ready) {
        return NULL;
    }
    return &lfs;
}
