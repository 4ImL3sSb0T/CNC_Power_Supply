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
int fifo_buffer_init(fifo_buffer_t *fifo, uint8_t *buffer, uint32_t size, void *mutex, fifo_buffer_mutex_lock_t lock, fifo_buffer_mutex_unlock_t unlock)
{
    if (!fifo || !buffer || size < 2) return -1; // size 是数组真实字节数，可用容量 size - 1

    fifo->buffer = buffer;
    fifo->size = size;
    fifo->head = 0;
    fifo->tail = 0;
    fifo->mutex = mutex;
    fifo->lock = lock;
    fifo->unlock = unlock;

    return 0;
}
#else
int fifo_buffer_init(fifo_buffer_t *fifo, uint8_t *buffer, uint32_t size)
{
    if (!fifo || !buffer || size < 2) return -1; // size 是数组真实字节数，可用容量 size - 1

    fifo->buffer = buffer;
    fifo->size = size;
    fifo->head = 0;
    fifo->tail = 0;

    return 0;
}
#endif

int fifo_buffer_write(fifo_buffer_t *fifo, const uint8_t *data, uint32_t length) {
    if (!fifo || !data || length == 0) return -1; // Invalid parameters

#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_lock(fifo);
#endif
//  可以用两段 memcpy 代替
    int left = fifo_buffer_get_left(fifo);
    if (left < 0 || (uint32_t)left < length) {
#ifdef FIFO_BUFFER_USING_MUTEX
        fifo_buffer_unlock(fifo);
#endif
        return -1; // Not enough space
    }

    for (size_t i = 0; i < length; ++i) {
        fifo->buffer[fifo->head] = data[i];
        fifo->head = (fifo->head + 1) % fifo->size;
    }
    
#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_unlock(fifo);
#endif

    return 0;
}

int fifo_buffer_read(fifo_buffer_t *fifo, uint8_t *data, uint32_t length) {
    if (!fifo || !data || length == 0) return -1; // Invalid parameters

#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_lock(fifo);
#endif

    int actual_read_length = fifo_buffer_get_used(fifo);
    if (actual_read_length == -1) {
#ifdef FIFO_BUFFER_USING_MUTEX
        fifo_buffer_unlock(fifo);
#endif
        return -1; // Invalid parameter
    }
    if (length < (uint32_t)actual_read_length) {
        actual_read_length = length;
    }
    
    for (size_t i = 0; i < (size_t)actual_read_length; ++i) {
        data[i] = fifo->buffer[fifo->tail];
        fifo->tail = (fifo->tail + 1) % fifo->size;
    }
    
#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_unlock(fifo);
#endif

    return actual_read_length;
}

int fifo_buffer_peek(fifo_buffer_t *fifo, uint8_t *data, uint32_t length, uint32_t offset) {
    if (!fifo || !data || length == 0) return -1; // Invalid parameters
#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_lock(fifo);
#endif

    int actual_read_length = fifo_buffer_get_used(fifo);
    if (actual_read_length == -1) {
#ifdef FIFO_BUFFER_USING_MUTEX
        fifo_buffer_unlock(fifo);
#endif
        return -1; // Invalid parameter
    }
    
    if (offset >= (uint32_t)actual_read_length) {
#ifdef FIFO_BUFFER_USING_MUTEX
        fifo_buffer_unlock(fifo);
#endif
        return 0; // Offset exceeds available data
    }

    if (length > (uint32_t)(actual_read_length - offset)) {
        length = actual_read_length - offset;
    }

    for (size_t i = 0; i < length; ++i) {
        data[i] = fifo->buffer[(fifo->tail + i + offset) % fifo->size];
    }

#ifdef FIFO_BUFFER_USING_MUTEX
    fifo_buffer_unlock(fifo);
#endif

    return length;
}

int fifo_buffer_get_left(fifo_buffer_t *fifo) {
    if (!fifo) return -1; // Invalid parameter
    return (fifo->size - 1) - fifo_buffer_get_used(fifo);
    
}

int fifo_buffer_get_used(fifo_buffer_t *fifo) {
    if (!fifo) return -1; // Invalid parameter
    return (fifo->head + fifo->size - fifo->tail) % fifo->size;
}