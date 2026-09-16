#include "fifo_buffer.h"

#ifdef FIFO_BUFFER_USING_MUTEX
static inline void fifo_buffer_lock(fifo_buffer_t *fifo) {
    if (fifo->lock) fifo->lock(fifo->mutex);
}

static inline void fifo_buffer_unlock(fifo_buffer_t *fifo) {
    if (fifo->unlock) fifo->unlock(fifo->mutex);
}
#endif

#ifdef FIFO_BUFFER_USING_MUTEX
exit_code_t fifo_buffer_init(fifo_buffer_t *fifo, u8 *buffer, u32 size, void *mutex, fifo_buffer_mutex_lock_t lock, fifo_buffer_mutex_unlock_t unlock)
{
    if (!fifo || !buffer || size < 2) return EXIT_FAIL; // size 是数组真实字节数，可用容量 size - 1

    fifo->buffer = buffer;
    fifo->size = size;
    fifo->head = 0;
    fifo->tail = 0;
    fifo->mutex = mutex;
    fifo->lock = lock;
    fifo->unlock = unlock;

    return EXIT_OK;
}
#else
exit_code_t fifo_buffer_init(fifo_buffer_t *fifo, u8 *buffer, u32 size)
{
    if (!fifo || !buffer || size < 2) return EXIT_FAIL; // size 是数组真实字节数，可用容量 size - 1

    fifo->buffer = buffer;
    fifo->size = size;
    fifo->head = 0;
    fifo->tail = 0;

    return EXIT_OK;
}
#endif

exit_code_t fifo_buffer_write(fifo_buffer_t *fifo, const u8 *data, u32 length) {
    if (!fifo || !data || length == 0) return EXIT_INVALID_PARAM;

#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_lock(fifo);
#endif
//  可以用两段 memcpy 代替
    i32 left = fifo_buffer_get_left(fifo);
    if (left < 0 || (u32)left < length) {
#ifdef FIFO_BUFFER_USING_MUTEX
        fifo_buffer_unlock(fifo);
#endif
        return (left < 0) ? EXIT_INVALID_PARAM : EXIT_NO_MEMORY; // Invalid parameter / Not enough space
    }

    for (u32 i = 0; i < length; ++i) {
        fifo->buffer[fifo->head] = data[i];
        fifo->head = (fifo->head + 1) % fifo->size;
    }
    
#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_unlock(fifo);
#endif

    return EXIT_OK;
}

i32 fifo_buffer_read(fifo_buffer_t *fifo, u8 *data, u32 length) {
    if (!fifo || !data || length == 0) return -1; // Invalid parameters

#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_lock(fifo);
#endif

    i32 actual_read_length = fifo_buffer_get_used(fifo);
    if (actual_read_length == -1) {
#ifdef FIFO_BUFFER_USING_MUTEX
        fifo_buffer_unlock(fifo);
#endif
        return -1; // Invalid parameter
    }
    if (length < (u32)actual_read_length) {
        actual_read_length = (i32)length;
    }
    
    for (u32 i = 0; i < (u32)actual_read_length; ++i) {
        data[i] = fifo->buffer[fifo->tail];
        fifo->tail = (fifo->tail + 1) % fifo->size;
    }
    
#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_unlock(fifo);
#endif

    return actual_read_length;
}

i32 fifo_buffer_peek(fifo_buffer_t *fifo, u8 *data, u32 length, u32 offset) {
    if (!fifo || !data || length == 0) return -1; // Invalid parameters
#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_lock(fifo);
#endif

    i32 actual_read_length = fifo_buffer_get_used(fifo);
    if (actual_read_length == -1) {
#ifdef FIFO_BUFFER_USING_MUTEX
        fifo_buffer_unlock(fifo);
#endif
        return -1; // Invalid parameter
    }
    
    if (offset >= (u32)actual_read_length) {
#ifdef FIFO_BUFFER_USING_MUTEX
        fifo_buffer_unlock(fifo);
#endif
        return 0; // Offset exceeds available data
    }

    if (length > ((u32)actual_read_length - offset)) {
        length = (u32)actual_read_length - offset;
    }

    for (u32 i = 0; i < length; ++i) {
        data[i] = fifo->buffer[(fifo->tail + i + offset) % fifo->size];
    }

#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_unlock(fifo);
#endif

    return (i32)length;
}

i32 fifo_buffer_get_left(fifo_buffer_t *fifo) {
    if (!fifo) return -1; // Invalid parameter
    return (i32)((fifo->size - 1) - (u32)fifo_buffer_get_used(fifo));
    
}

i32 fifo_buffer_get_used(fifo_buffer_t *fifo) {
    if (!fifo) return -1; // Invalid parameter
    return (i32)((fifo->head + fifo->size - fifo->tail) % fifo->size);
}