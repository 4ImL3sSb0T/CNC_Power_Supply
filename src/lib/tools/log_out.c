#include "lib/tools/log_out.h"

#include <stdio.h>

#include "hardware/sync.h"
#include "pico/stdio_usb.h"

/* 一次 drain 最多吐这么多字节。吐得太多会把链路任务堵在 CDC FIFO 上
 * （一次几毫秒），命令响应和遥测都会跟着迟到，所以分摊到多轮里慢慢吐 */
#define LOG_DRAIN_MAX   512u

/* ---------------- 环形缓冲 ---------------- */

static char   s_buf[LOG_BUF_SIZE];
static size_t s_head;       /* 下一个写入位置 */
static size_t s_tail;       /* 下一个读出位置 */
static size_t s_count;      /* 缓冲里现有字节数 */
static size_t s_dropped;    /* 缓冲满被丢掉的字节数，drain 时报一次就清零 */

/* 生产者（任意上下文）与消费者（链路任务）共用这几个指针，用关中断保护。
 * 临界区只有几十字节的拷贝，几微秒量级 */
static void buf_push(const char *data, size_t n)
{
    uint32_t save = save_and_disable_interrupts();
    size_t i;

    for (i = 0; i < n; i++) {
        if (s_count == LOG_BUF_SIZE) {
            /* 满了：丢最旧的。留下刚发生的事 —— 那才是要看重启原因的人关心的 */
            s_tail = (s_tail + 1u) % LOG_BUF_SIZE;
            s_count--;
            s_dropped++;
        }
        s_buf[s_head] = data[i];
        s_head = (s_head + 1u) % LOG_BUF_SIZE;
        s_count++;
    }

    restore_interrupts(save);
}

void log_printf(const char *fmt, ...)
{
    char body[LOG_LINE_MAX];
    char line[LOG_LINE_MAX + sizeof(LOG_LINE_PREFIX)];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;                     /* 格式化失败：宁可整条不输出，也不吐半截 */
    }

    /* 前缀、正文、换行先拼成一整行再入缓冲：一次推入保证这一行不会被别的任务
     * 插进来的字节劈开，上位机按行识别前缀才不会漏 */
    n = snprintf(line, sizeof(line), LOG_LINE_PREFIX "%s\n", body);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len > sizeof(line) - 1u) {
            len = sizeof(line) - 1u;    /* snprintf 截断时返回的是"想要"的长度 */
        }
        buf_push(line, len);
    }
}

size_t log_drain(void)
{
    char chunk[64];
    size_t total = 0;
    size_t dropped;
    uint32_t save;

    /* 主机没把 DTR 拉起来时，stdio_usb 会把每个字节默默扔掉。吐了等于没吐，
     * 留着等下一轮 —— 这正是缓冲存在的意义 */
    if (!stdio_usb_connected()) {
        return 0u;
    }

    save = save_and_disable_interrupts();
    dropped = s_dropped;
    s_dropped = 0u;
    restore_interrupts(save);

    if (dropped > 0u) {
        printf(LOG_LINE_PREFIX "[log] 缓冲满，丢弃了 %u 字节最旧的日志\n", (unsigned)dropped);
    }

    while (total < LOG_DRAIN_MAX) {
        size_t want = sizeof(chunk);
        size_t n = 0;

        if (want > LOG_DRAIN_MAX - total) {
            want = LOG_DRAIN_MAX - total;
        }

        save = save_and_disable_interrupts();
        while (n < want) {
            if (s_count == 0u) {
                break;
            }
            chunk[n++] = s_buf[s_tail];
            s_tail = (s_tail + 1u) % LOG_BUF_SIZE;
            s_count--;
        }
        restore_interrupts(save);

        if (n == 0u) {
            break;
        }
        fwrite(chunk, 1u, n, stdout);
        total += n;
    }

    return total;
}
