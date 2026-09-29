/**
 * @file        fs.h
 * @brief       LittleFS 对外接口。块设备细节留在 fs.c，调用方不要直接碰 Flash
 */
#ifndef FS_H
#define FS_H

#include "lfs.h"

/* 只能在调度器启动之后、从任务里调用：首次挂载需要格式化时会写 Flash，
 * 而写 Flash 靠 flash_safe_execute 在另一核上临时建任务来保证安全（详见 fs.c）。 */
int fs_init(void);

/* littlefs 已按 LFS_THREADSAFE 编译，每个 lfs_* 公开 API 进来就自动取锁，
 * 跨任务、跨核直接调用是安全的。这个 handle 不需要调用方再加锁。 */
lfs_t *fs_get_handle(void);

/* 把多次 lfs_* 调用合成一次原子操作时用（递归锁，可嵌套）。 */
int fs_lock(void);
int fs_unlock(void);

#endif
