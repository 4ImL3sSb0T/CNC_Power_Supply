/**
 * @file        fs_test.c
 * @brief       LittleFS 冒烟测试：只跑一次，结果看串口，跑完自删
 *
 * 流程：fs_init() 挂载 → mkdir /log → 读改写 /log/bootcount.txt → 列 /log → 退出。
 * 首次上电或文件系统损坏时 fs.c 会格式化整个分区，所以这一路是自愈的；
 * 判断 Flash 读写真的通了，看 boot count 是否逐次递增、且掉电后不回退即可。
 *
 * 必须在调度器启动后由任务调用 fs_init()：写 Flash 走 flash_safe_execute，
 * 它靠在另一核上现场建任务来保证安全（见 src/service/fs/fs.c 文件头）。
 */

#include "app/fs_test/fs_test.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "service/fs/fs.h"

#define FS_TEST_DIR   "/log"
#define FS_TEST_FILE  "/log/bootcount.txt"

static void fs_test_list(lfs_t *lfs, const char *path)
{
    lfs_dir_t dir;
    struct lfs_info info;

    if (lfs_dir_open(lfs, &dir, path) != LFS_ERR_OK) {
        printf("[fs] 打不开目录 %s\n", path);
        return;
    }
    while (lfs_dir_read(lfs, &dir, &info) > 0) {
        if (info.name[0] == '.') {
            continue;   /* "." 与 ".." 是 littlefs 合成的，不是真条目 */
        }
        printf("[fs]   %s %s  %u B\n",
               (info.type == LFS_TYPE_DIR) ? "DIR " : "FILE", info.name,
               (unsigned)info.size);
    }
    lfs_dir_close(lfs, &dir);
}

static void fs_test_run(void)
{
    lfs_file_t file;
    uint32_t boot_count = 0;
    lfs_t *lfs;
    int err;

    err = fs_init();
    if (err != LFS_ERR_OK) {
        printf("[fs] fs_init 失败: %d\n", err);
        return;
    }

    lfs = fs_get_handle();

    err = lfs_mkdir(lfs, FS_TEST_DIR);
    if (err != LFS_ERR_OK && err != LFS_ERR_EXIST) {
        printf("[fs] mkdir %s 失败: %d\n", FS_TEST_DIR, err);
        return;
    }

    /* mkdir/打开/读改写当一个原子操作做完，不让别的任务插在中间看到半截状态 */
    fs_lock();
    err = lfs_file_open(lfs, &file, FS_TEST_FILE, LFS_O_RDWR | LFS_O_CREAT);
    if (err == LFS_ERR_OK) {
        if (lfs_file_read(lfs, &file, &boot_count, sizeof(boot_count))
                < (lfs_ssize_t)sizeof(boot_count)) {
            boot_count = 0;   /* 文件是新建的或短读，从 0 重新计 */
        }
        boot_count++;
        lfs_file_rewind(lfs, &file);
        err = lfs_file_write(lfs, &file, &boot_count, sizeof(boot_count))
              < (lfs_ssize_t)sizeof(boot_count) ? LFS_ERR_IO : LFS_ERR_OK;
        /* 写入是先落在文件的写缓存里，真正的 Flash 编程发生在 close，
         * 所以这里的返回值必须看 close 的，光看 write 会把失败当成功 */
        if (lfs_file_close(lfs, &file) != LFS_ERR_OK && err == LFS_ERR_OK) {
            err = LFS_ERR_IO;
        }
    }
    fs_unlock();

    if (err != LFS_ERR_OK) {
        printf("[fs] %s 读改写失败: %d\n", FS_TEST_FILE, err);
        return;
    }

    printf("[fs] 挂载 OK，已用 %u 块 | boot count = %u\n",
           (unsigned)lfs_fs_size(lfs), (unsigned)boot_count);
    fs_test_list(lfs, FS_TEST_DIR);
}

void fs_test_task(void *pvParameters)
{
    (void)pvParameters;

    fs_test_run();

    /* 一次性任务，跑完腾出栈；文件系统保持挂载，后续任务用 fs_get_handle() 直接访问 */
    vTaskDelete(NULL);
}
