/*
 * FreeRTOSConfig.h — CNC_Power_Supply
 *
 * 目标平台：Raspberry Pi Pico 2 (RP2350, ARM Cortex-M33, 双核)
 * 内核：    FreeRTOS/FreeRTOS-Kernel V11.3.1
 * 移植：    FreeRTOS-Kernel-Community-Supported-Ports 的 GCC/RP2350_ARM_NTZ (SMP)
 *
 * 移植的必需项已在下方“Armv8-M / 移植必需项”一节中固定，改动前请先读
 * src/third_party/FreeRTOS-Kernel-Community-Supported-Ports/GCC/RP2350_ARM_NTZ/README.md。
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*---------------------------------------------------------------------------*/
/* 调度器基础配置                                                             */
/*---------------------------------------------------------------------------*/

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1

/* 同优先级任务之间的时间片轮转；SMP 下若设为 1 则允许两个核心同时跑
 * 不同优先级就绪任务，能提高吞吐但降低确定性。本项目的控制环路更看重
 * 时序确定性，故保持为 0。 */
#define configRUN_MULTIPLE_PRIORITIES           0

/* SMP：RP2350 有 2 个 Cortex-M33 核。设为 1 即退化为单核调度。 */
#define configNUMBER_OF_CORES                   2
#define configTICK_CORE                         0
#define configUSE_CORE_AFFINITY                 1
#define configUSE_PASSIVE_IDLE_HOOK             0

#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE                 0   /* RP2350 移植尚未支持 tickless */
#define configTICK_RATE_HZ                      ( 1000 )   /* 1 kHz 节拍，1 ms 分辨率 */

#define configMAX_PRIORITIES                    ( 8 )
#define configMINIMAL_STACK_SIZE                ( 256 )    /* 单位：字(word)，即 1 KiB */
#define configMAX_TASK_NAME_LEN                 ( 16 )
/* tick 计数宽度。旧的 configUSE_16_BIT_TICKS 在 V11.x 里仍被支持（FreeRTOS.h
 * 会做映射），但已不推荐，新写法是直接指定宽度。两者只能存在一个。 */
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1

/*---------------------------------------------------------------------------*/
/* 同步原语 / 软件定时器                                                       */
/*---------------------------------------------------------------------------*/

#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    1
#define configUSE_TASK_NOTIFICATIONS            1

#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            ( configMINIMAL_STACK_SIZE * 2 )

#define configUSE_STREAM_BUFFERS                1
#define configUSE_EVENT_GROUPS                  1

/*---------------------------------------------------------------------------*/
/* 内存分配                                                                    */
/*---------------------------------------------------------------------------*/
/* 动态分配走 FreeRTOS-Kernel-Heap4（见 CMakeLists.txt 的 target_link_libraries）。
 * RP2350 共有 520 KiB SRAM，此处给内核堆 96 KiB，其余留给栈/静态数据。 */
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   ( 96 * 1024 )
#define configAPPLICATION_ALLOCATED_HEAP        0

/* 静态分配：可以用 xTaskCreateStatic() 等 API，任务不占上面那块 heap。
 * configKERNEL_PROVIDED_STATIC_MEMORY 让内核自带 idle / passive-idle / timer
 * 任务的静态内存，省掉自己实现 vApplicationGetIdleTaskMemory、
 * vApplicationGetPassiveIdleTaskMemory（SMP 才有）、vApplicationGetTimerTaskMemory
 * 三个函数。这两个宏由本文件统一定义，不要改用 -D 传，否则会重定义。 */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configKERNEL_PROVIDED_STATIC_MEMORY     1

/* newlib 可重入：置 1 会让每个任务持有独立的 struct _reent（errno、strtok 等
 * 才真正线程安全），代价是每个任务多占约 100 字节。若暂时不需要可置 0。 */
#define configUSE_NEWLIB_REENTRANT              1

/*---------------------------------------------------------------------------*/
/* 钩子函数                                                                    */
/*---------------------------------------------------------------------------*/
/* 置 1 后需要在应用侧实现对应函数，见 src/app/freertos_hooks.c */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/*---------------------------------------------------------------------------*/
/* 运行时统计与追踪                                                             */
/*---------------------------------------------------------------------------*/
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/*---------------------------------------------------------------------------*/
/* 协程（基本已废弃，不要开启）                                                  */
/*---------------------------------------------------------------------------*/
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         ( 2 )

/*---------------------------------------------------------------------------*/
/* Armv8-M / 移植必需项（RP2350_ARM_NTZ README 指定，勿随意改动）                */
/*---------------------------------------------------------------------------*/
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1

/* 未使用浮点运算可置 0，能省下每任务约 132 字节的 FPU 上下文保存开销。
 * 本项目用到浮点，故开启。 */
#define configENABLE_FPU                        1

/* README 注明：当前移植只验证过这一个取值。 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    ( 16 )

/*---------------------------------------------------------------------------*/
/* 断言                                                                        */
/*---------------------------------------------------------------------------*/
extern void vApplicationAssertFailed( const char * pcFile, unsigned long ulLine );

/* 注意用 portDISABLE_INTERRUPTS 而非 taskDISABLE_INTERRUPTS：
 * configASSERT 会在 portmacro.h 内部（vPortRecursiveLock）展开，那时 task.h 还没进来。 */
#define configASSERT( x )                                            \
    if( ( x ) == 0 )                                                 \
    {                                                                \
        portDISABLE_INTERRUPTS();                                    \
        vApplicationAssertFailed( __FILE__, __LINE__ );              \
        for( ; ; )                                                   \
        ;                                                            \
    }

/*---------------------------------------------------------------------------*/
/* 可裁剪的内核 API                                                            */
/*---------------------------------------------------------------------------*/
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xSemaphoreGetMutexHolder        1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_eTaskGetState                   1

/* 注意：configUSE_TRACE_FACILITY 为 0 时，event_groups.h 会把
 * xEventGroupSetBitsFromISR() 展开成 xTimerPendFunctionCallFromISR()，
 * 因此这一项必须为 1，否则 port.c 编译不过。 */
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTimerGetTimerDaemonTaskHandle  1

/*---------------------------------------------------------------------------*/
/* 中断优先级                                                                  */
/*---------------------------------------------------------------------------*/
/* 这个移植只认上面那一个 configMAX_SYSCALL_INTERRUPT_PRIORITY。
 * 通用 Cortex-M 移植常见的 configKERNEL_INTERRUPT_PRIORITY、
 * configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY、configPRIO_BITS 在本移植里
 * 都没有被引用，定义了也不会生效（优先级位数是 port.c 启动时探测 SHPR2 得出的），
 * 所以这里不写，免得和上面那个真实生效的值混淆。 */

#endif /* FREERTOS_CONFIG_H */
