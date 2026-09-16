#ifndef _COMMON_DEF_H_
#define _COMMON_DEF_H_

#ifdef __cplusplus
extern "C" {
#endif
typedef enum exit_code_t
{
    EXIT_SKIP = 1,                    // 跳过/不处理
    EXIT_IN_PROGRESS = 2,             // 异步任务已启动，后续会返回最终结果
    EXIT_OK = 0,                       // 成功/正常 - 嵌入式常用
    EXIT_FAIL = -1,                    // 通用失败 - 嵌入式常用
    EXIT_TIMEOUT = -2,                 // 超时错误 - 嵌入式常用
    EXIT_INVALID_PARAM = -3,           // 无效参数 - 嵌入式常用
    EXIT_NOT_SUPPORTED = -4,           // 不支持的操作 - 嵌入式常用
    EXIT_NO_MEMORY = -5,               // 内存不足 - 嵌入式常用
    EXIT_BUSY = -6,                    // 设备忙/资源被占用 - 嵌入式常用
    EXIT_NO_RESOURCE = -7,             // 资源不足 - 嵌入式常用
    EXIT_ALREADY_EXISTS = -8,          // 资源已存在
    EXIT_DOES_NOT_EXIST = -9,          // 资源不存在
    EXIT_NOT_INITIALIZED = -10,        // 未初始化 - 嵌入式常用
    EXIT_ALREADY_INITIALIZED = -11,    // 已经初始化
    EXIT_CRC_MISMATCH = -12,           // CRC校验失败 - 嵌入式常用
    EXIT_HW_FAILURE = -13,             // 硬件故障 - 嵌入式常用
    EXIT_UNKNOWN = -14,                // 未知错误
} exit_code_t;

#define EXIT_CODE_NAME_CASE(code_item) case code_item: return #code_item

typedef struct cmd_exec_result_t
{
    exit_code_t code;
    const char *ctx;   // Optional machine-readable context (token/KV)
} cmd_exec_result_t;

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;
typedef int i32;
typedef short i16;
typedef char i8;
typedef long long i64;
typedef float f32;
typedef double f64;

typedef volatile unsigned int vu32;
typedef volatile unsigned short vu16;
typedef volatile unsigned char vu8;
typedef volatile unsigned long long vu64;
typedef volatile int vi32;
typedef volatile short vi16;
typedef volatile char vi8;
typedef volatile long long vi64;
typedef volatile float vf32;
typedef volatile double vf64;

void assert_fun(exit_code_t code);
const char *error_code_name(exit_code_t code);

static inline cmd_exec_result_t cmd_exec_make(exit_code_t code, const char *ctx)
{
    cmd_exec_result_t res = {
        .code = code,
        .ctx = ctx
    };
    return res;
}

#define CMD_EXEC_CODE(code_value) cmd_exec_make((code_value), NULL)
#define CMD_EXEC_CTX(code_value, ctx_value) cmd_exec_make((code_value), (ctx_value))

#ifdef __cplusplus
}
#endif
#endif

