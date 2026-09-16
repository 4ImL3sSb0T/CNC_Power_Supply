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
