/*
 * FreeRTOSConfig.h - STM32F401RE (Cortex-M4F, 84 MHz, 96 KB SRAM, 512 KB flash)
 * Kernel: FreeRTOS V11.3.x from ../support, port GCC/ARM_CM4F, heap_4.
 * FreeRTOS is NOT enabled in CubeMX; this file is hand-maintained.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#if !defined(__ASSEMBLER__) && !defined(__IAR_SYSTEMS_ASM__)
  #include <stdint.h>
  extern uint32_t SystemCoreClock;          /* 84 000 000 after SystemClock_Config() */
  void vAssertCalled(const char *file, int line);
#endif

/* ------------------------------------------------------------------------
 * Scheduler
 * --------------------------------------------------------------------- */
#define configUSE_PREEMPTION                      1
#define configUSE_TIME_SLICING                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION   1   /* CLZ-based, max 32 prios */
#define configUSE_TICKLESS_IDLE                   0
#define configCPU_CLOCK_HZ                        (SystemCoreClock)
#define configTICK_RATE_HZ                        1000 /* 1 ms tick: lwIP timers and
                                                          the 50 ms debounce need ms resolution */
#define configMAX_PRIORITIES                      7    /* see app_config.h */
#define configMINIMAL_STACK_SIZE                  128  /* words, idle task */
#define configMAX_TASK_NAME_LEN                   12
#define configTICK_TYPE_WIDTH_IN_BITS             TICK_TYPE_WIDTH_32_BITS /* lwIP port needs 32-bit ticks */
#define configIDLE_SHOULD_YIELD                   1
#define configUSE_TASK_NOTIFICATIONS              1    /* ISR -> task wake-ups (ENC INT, B1) */
#define configTASK_NOTIFICATION_ARRAY_ENTRIES     1
#define configQUEUE_REGISTRY_SIZE                 0
#define configENABLE_BACKWARD_COMPATIBILITY       1    /* lwIP sys_arch.c uses portTICK_RATE_MS */
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS   0    /* LWIP_NETCONN_SEM_PER_THREAD = 0 */
#define configUSE_NEWLIB_REENTRANT                0    /* stdio goes through app_log (mutex) */
#define configSTACK_DEPTH_TYPE                    uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE          size_t

/* ------------------------------------------------------------------------
 * Synchronisation primitives
 * --------------------------------------------------------------------- */
#define configUSE_MUTEXES                         1    /* SPI mutex, printf mutex, lwIP */
#define configUSE_RECURSIVE_MUTEXES               1    /* lwIP sys_mutex_new() / core lock */
#define configUSE_COUNTING_SEMAPHORES             1
#define configUSE_QUEUE_SETS                      0
#define configUSE_TIMERS                          0    /* nothing uses software timers;
                                                          saves the timer task and its stack */
#define configUSE_CO_ROUTINES                     0
#define configUSE_EVENT_GROUPS                    0
#define configUSE_STREAM_BUFFERS                  0

/* ------------------------------------------------------------------------
 * Memory: heap_4, all kernel objects allocated dynamically
 *
 * RAM budget (96 KB):
 *   FreeRTOS heap (this)            32 KB  task stacks (~11 KB), TCBs, queues,
 *                                          lwIP mboxes/semaphores, headroom
 *   lwIP MEM_SIZE + PBUF pool + memp ~26 KB (static .bss, see lwipopts.h)
 *   ENC/app buffers, HAL, .data      ~3 KB
 *   MSP stack (ISRs after start)      1 KB (linker _Min_Stack_Size)
 *   newlib heap                     0.5 KB (_Min_Heap_Size)
 *   => ~63 KB used, ~33 KB spare for growth / debugging.
 * --------------------------------------------------------------------- */
#define configSUPPORT_DYNAMIC_ALLOCATION          1    /* required by lwIP port */
#define configSUPPORT_STATIC_ALLOCATION           0
#define configTOTAL_HEAP_SIZE                     ((size_t)(32 * 1024))
#define configAPPLICATION_ALLOCATED_HEAP          0

/* ------------------------------------------------------------------------
 * Development checks (turn off / reduce for production)
 * --------------------------------------------------------------------- */
#define configCHECK_FOR_STACK_OVERFLOW            2    /* pattern check at every switch */
#define configUSE_MALLOC_FAILED_HOOK              1
#define configUSE_IDLE_HOOK                       0
#define configUSE_TICK_HOOK                       0
#define configUSE_DAEMON_TASK_STARTUP_HOOK        0
#define configRECORD_STACK_HIGH_ADDRESS           1
#define configUSE_TRACE_FACILITY                  0
#define configGENERATE_RUN_TIME_STATS             0
#define configUSE_STATS_FORMATTING_FUNCTIONS      0
#define configCHECK_HANDLER_INSTALLATION          1    /* asserts if SVC/PendSV are not
                                                          the kernel's handlers */

#define configASSERT(x)  do { if ((x) == 0) { vAssertCalled(__FILE__, __LINE__); } } while (0)

/* ------------------------------------------------------------------------
 * Cortex-M interrupt priorities
 *
 * STM32F4 implements 4 priority bits; CubeMX uses NVIC_PRIORITYGROUP_4
 * (all bits pre-emption).  Lower number = more urgent.
 *   0..4   never masked by the kernel, must NOT call any FreeRTOS API
 *   5..15  may call ...FromISR() APIs
 * Required NVIC settings (CubeMX > NVIC):
 *   EXTI line0 (ENC INT)         preemption 6
 *   EXTI line[15:10] (B1)        preemption 7
 *   TIM5 (HAL timebase)          preemption 15 (TICK_INT_PRIORITY, unchanged)
 * --------------------------------------------------------------------- */
#define configPRIO_BITS                                 4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5
#define configKERNEL_INTERRUPT_PRIORITY \
        (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
        (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_API_CALL_INTERRUPT_PRIORITY     configMAX_SYSCALL_INTERRUPT_PRIORITY

/* ------------------------------------------------------------------------
 * Optional API functions
 * --------------------------------------------------------------------- */
#define INCLUDE_vTaskDelay                        1    /* required by lwIP port */
#define INCLUDE_vTaskSuspend                      1    /* required by lwIP port (portMAX_DELAY waits) */
#define INCLUDE_xTaskDelayUntil                   1
#define INCLUDE_vTaskDelete                       0
#define INCLUDE_vTaskPrioritySet                  0
#define INCLUDE_uxTaskPriorityGet                 0
#define INCLUDE_xTaskGetSchedulerState            1    /* app_log: no mutex before start */
#define INCLUDE_xTaskGetCurrentTaskHandle         1    /* lwIP core-locking checks */
#define INCLUDE_uxTaskGetStackHighWaterMark       1    /* heartbeat diagnostics */
#define INCLUDE_xTaskGetIdleTaskHandle            0
#define INCLUDE_eTaskGetState                     0
#define INCLUDE_xTimerPendFunctionCall            0

/* ------------------------------------------------------------------------
 * Map the port's handlers onto the CMSIS vector names used by
 * startup_stm32f401retx.s.  CubeMX must NOT generate these three handlers:
 * NVIC > Code generation > untick "Generate IRQ handler" for
 * "System service call via SWI instruction", "Pendable request for system
 * service" and "System tick timer".
 * --------------------------------------------------------------------- */
#define vPortSVCHandler      SVC_Handler
#define xPortPendSVHandler   PendSV_Handler
#define xPortSysTickHandler  SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
