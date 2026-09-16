/*
 * FreeRTOS 钩子函数实现。
 *
 * FreeRTOSConfig.h 中以下配置被置 1，因此必须提供对应的实现，
 * 否则会在链接阶段报 undefined reference：
 *   configCHECK_FOR_STACK_OVERFLOW  -> vApplicationStackOverflowHook()
 *   configUSE_MALLOC_FAILED_HOOK    -> vApplicationMallocFailedHook()
 *   configASSERT                    -> vApplicationAssertFailed()
 */

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "pico/time.h"

void vApplicationStackOverflowHook( TaskHandle_t xTask, char * pcTaskName )
{
    ( void ) xTask;

    taskDISABLE_INTERRUPTS();
    printf( "!! stack overflow in task \"%s\"\n", pcTaskName );

    for( ; ; )
    {
    }
}

void vApplicationMallocFailedHook( void )
{
    taskDISABLE_INTERRUPTS();
    printf( "!! pvPortMalloc failed (configTOTAL_HEAP_SIZE = %u)\n",
            ( unsigned ) configTOTAL_HEAP_SIZE );

    for( ; ; )
    {
    }
}

void vApplicationAssertFailed( const char * pcFile, unsigned long ulLine )
{
    printf( "!! configASSERT failed at %s:%lu\n", pcFile, ulLine );
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
