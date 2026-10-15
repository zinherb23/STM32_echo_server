/*
 * rtos_hooks.c
 */
#include "FreeRTOS.h"
#include "task.h"
#include "app_log.h"

/* configCHECK_FOR_STACK_OVERFLOW = 2. Called from the context switch
 * (PendSV), so it must not use the normal printf. */
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    app_panic("stack overflow in task", name, 0);
}

/* configUSE_MALLOC_FAILED_HOOK = 1. pvPortMalloc() returned NULL. */
void vApplicationMallocFailedHook(void)
{
    app_panic("pvPortMalloc failed, heap free bytes", NULL,
              (uint32_t)xPortGetFreeHeapSize());
}

/* configASSERT() target, also used by LWIP_PLATFORM_ASSERT (cc.h). */
void vAssertCalled(const char *file, int line)
{
    app_panic("assert", file, (uint32_t)line);
}
