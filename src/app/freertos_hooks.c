/*
 * FreeRTOS 钩子函数实现。
 *
 * FreeRTOSConfig.h 中以下配置被置 1，因此必须提供对应的实现，
 * 否则会在链接阶段报 undefined reference：
 *   configCHECK_FOR_STACK_OVERFLOW  -> vApplicationStackOverflowHook()
 *   configUSE_MALLOC_FAILED_HOOK    -> vApplicationMallocFailedHook()
 *   configASSERT                    -> vApplicationAssertFailed()
 */

#include "FreeRTOS.h"
#include "task.h"

#include "lib/tools/log_out.h"
#include "lib/tools/reset_reason.h"
#include "pico/time.h"

void vApplicationStackOverflowHook( TaskHandle_t xTask, char * pcTaskName )
{
    ( void ) xTask;

    taskDISABLE_INTERRUPTS();
    /* 先存证据再打印：打印要走 stdio，这条路在这个上下文里不一定还活着 */
    reset_reason_mark( RESET_MARK_STACK_OVERFLOW, pcTaskName, 0u );
    log_printf( "!! stack overflow in task \"%s\"", pcTaskName );
    /* 马上要死循环，没人再有机会 drain 了 —— 趁现在把这条推出去 */
    ( void ) log_drain();

    for( ; ; )
    {
    }
}

void vApplicationMallocFailedHook( void )
{
    taskDISABLE_INTERRUPTS();
    reset_reason_mark( RESET_MARK_MALLOC_FAILED, NULL, ( uint32_t ) configTOTAL_HEAP_SIZE );
    log_printf( "!! pvPortMalloc failed (configTOTAL_HEAP_SIZE = %u)",
            ( unsigned ) configTOTAL_HEAP_SIZE );
    ( void ) log_drain();

    for( ; ; )
    {
    }
}

void vApplicationAssertFailed( const char * pcFile, unsigned long ulLine )
{
    /* 注意：这条路径**不会**重启，只留个标记就返回（configASSERT 的语义是"记一笔
     * 然后继续跑"）。所以下次复位时若看到 ASSERT 标记，说明断言之后又出了别的事 */
    reset_reason_mark( RESET_MARK_ASSERT, pcFile, ( uint32_t ) ulLine );
    log_printf( "!! configASSERT failed at %s:%lu", pcFile, ulLine );
}

/*
 * 运行时间统计（FreeRTOSConfig.h 中 configGENERATE_RUN_TIME_STATS == 1）：
 * 内核在任务切换时调用 ulPortGetRunTimeCounterValue()，把实际运行时间累加到
 * 每个任务的 ulRunTimeCounter（阻塞等待不计入），供 CPU 占用率统计使用。
 */
uint32_t ulPortGetRunTimeCounterValue( void )
{
    return time_us_32(); /* 1 MHz 自由运行计数，µs 分辨率 */
}

void vConfigureTimerForRunTimeStats( void )
{
    /* RP2350 硬件定时器复位后即自由运行，无需配置。 */
}
