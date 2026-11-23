#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* PY32F003F18P6: Cortex-M0+, 24MHz HSI, 8KB SRAM -> Static-only FreeRTOS. */

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE                 0

#define configCPU_CLOCK_HZ                      (24000000UL)
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    4
#define configMINIMAL_STACK_SIZE                ((configSTACK_DEPTH_TYPE)96)
#define configMAX_TASK_NAME_LEN                 12
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1

/* Zero dynamic heap: every task, queue, and mutex is statically allocated. */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        0
#define configKERNEL_PROVIDED_STATIC_MEMORY     1

#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_TIMERS                        0
#define configUSE_EVENT_GROUPS                  0
#define configUSE_STREAM_BUFFERS                0
#define configUSE_QUEUE_SETS                    0
#define configUSE_CO_ROUTINES                   0

#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configCHECK_FOR_STACK_OVERFLOW          2

#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* Cortex-M0+ NVIC uses top 2 bits (4 priority levels: 0..3). */
#define configKERNEL_INTERRUPT_PRIORITY         (3U << 6U)
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (1U << 6U)

#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    0
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetSchedulerState          0

/* Map CMSIS vector names directly to FreeRTOS Cortex-M0 port handlers. */
#define vPortSVCHandler                         SVC_Handler
#define xPortPendSVHandler                      PendSV_Handler
#define xPortSysTickHandler                     SysTick_Handler

#define configASSERT(x)                         do { if ((x) == 0) { __disable_irq(); for (;;) {} } } while (0)

#endif /* FREERTOS_CONFIG_H */
