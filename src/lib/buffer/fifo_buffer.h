#ifndef FIFO_BUFFER_H
#define FIFO_BUFFER_H

#include "lib/tools/common_def.h"

#ifdef FIFO_BUFFER_USING_MUTEX
typedef void (*fifo_buffer_mutex_lock_t)(void *mutex);
typedef void (*fifo_buffer_mutex_unlock_t)(void *mutex);
#endif

/* 环形缓冲：size 是底层数组的真实字节数，可用容量只有 size - 1（留一格区分空/满），所以 size 至少为 2。
   head == tail 表示空。跨线程/中断共用必须开 FIFO_BUFFER_USING_MUTEX，且该宏要全局一致：它会改变结构体布局。 */
typedef struct {
    u8 *buffer;
    u32 size;
    u32 head; // write index
    u32 tail; // read index
#ifdef FIFO_BUFFER_USING_MUTEX
    void *mutex;
    fifo_buffer_mutex_lock_t lock;
    fifo_buffer_mutex_unlock_t unlock;
#endif
} fifo_buffer_t;

#ifdef FIFO_BUFFER_USING_MUTEX
exit_code_t fifo_buffer_init(fifo_buffer_t *fifo, u8 *buffer, u32 size, void *mutex, fifo_buffer_mutex_lock_t lock, fifo_buffer_mutex_unlock_t unlock);
#else
exit_code_t fifo_buffer_init(fifo_buffer_t *fifo, u8 *buffer, u32 size);
#endif

/* 空间不足时整块拒绝：一个字节都不写入。成功返回 EXIT_OK，失败返回 EXIT_INVALID_PARAM / EXIT_NO_MEMORY */
exit_code_t fifo_buffer_write(fifo_buffer_t *fifo, const u8 *data, u32 length);

/* 最多读走 min(length, 已存字节数)。返回实际读取长度，可能小于 length（短读不报错），负值才是错误 */
i32 fifo_buffer_read(fifo_buffer_t *fifo, u8 *data, u32 length);

/* 不消费数据。返回实际拷贝长度；0 表示无数据或 offset 已超出已存数据范围 */
i32 fifo_buffer_peek(fifo_buffer_t *fifo, u8 *data, u32 length, u32 offset);

i32 fifo_buffer_get_left(fifo_buffer_t *fifo);

i32 fifo_buffer_get_used(fifo_buffer_t *fifo);

#endif /* FIFO_BUFFER_H */