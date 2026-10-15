/*
 * arch/cc.h - lwIP compiler/platform glue for STM32F401 + GCC + newlib.
 */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>          /* struct timeval (LWIP_TIMEVAL_PRIVATE = 0) */
#include "app_log.h"

/* errno values and the errno variable come from newlib's <errno.h>. */
#define LWIP_ERRNO_STDINCLUDE  1

/* Diagnostics go through the mutex-guarded printf (task context only). */
#define LWIP_PLATFORM_DIAG(x)  do { printf x; } while (0)

/* lwIP assertion: print via the panic path and halt. */
#define LWIP_PLATFORM_ASSERT(x) app_panic("lwIP assert", (x), __LINE__)

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT     __attribute__((packed))
#define PACK_STRUCT_FIELD(x)   x

#endif /* LWIP_ARCH_CC_H */
