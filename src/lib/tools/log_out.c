#include "lib/tools/log_out.h"

#include <stdio.h>

void log_printf(const char *fmt, ...) {
    char body[LOG_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;                     /* 格式化失败：宁可整条不输出，也不吐半截 */
    }

    /* 先拼好再一次性写出。stdio_usb 不做缓冲，一次 printf 内部也是分段下发；
     * 若直接分成"前缀 / 正文 / 换行"三次调用，中间正好被 psu_link 发帧插进来，
     * 上位机看到的就是一条没有前缀的残行。 */
    printf(LOG_LINE_PREFIX "%s\n", body);
}
