/*
 * app_log.c
 */
#include "app_log.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart2;

#define LOG_LINE_MAX      256U
#define LOG_UART_TIMEOUT  100U   /* ms per chunk; 256 B at 115200 = 22 ms */

static SemaphoreHandle_t log_mutex;
static char log_buf[LOG_LINE_MAX];
static volatile uint32_t log_dropped;

static int in_isr(void)
{
    return __get_IPSR() != 0U;
}

static int scheduler_running(void)
{
    return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

/* Returns 1 if the caller may print (and holds the mutex when required). */
static int log_lock(void)
{
    if (in_isr()) {
        log_dropped++;
        return 0;
    }
    if (log_mutex != NULL && scheduler_running()) {
        (void)xSemaphoreTake(log_mutex, portMAX_DELAY);
    }
    return 1;
}

static void log_unlock(void)
{
    if (log_mutex != NULL && scheduler_running()) {
        (void)xSemaphoreGive(log_mutex);
    }
}

/* Sends len bytes, expanding a bare '\n' into "\r\n". Mutex must be held. */
static void log_emit(const char *s, size_t len)
{
    static const uint8_t crlf[2] = { '\r', '\n' };
    size_t start = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) {
            if (i > start) {
                HAL_UART_Transmit(&huart2, (const uint8_t *)&s[start],
                                  (uint16_t)(i - start), LOG_UART_TIMEOUT);
            }
            HAL_UART_Transmit(&huart2, crlf, 2, LOG_UART_TIMEOUT);
            start = i + 1;
        }
    }
    if (len > start) {
        HAL_UART_Transmit(&huart2, (const uint8_t *)&s[start],
                          (uint16_t)(len - start), LOG_UART_TIMEOUT);
    }
}

void app_log_init(void)
{
    if (log_mutex == NULL) {
        log_mutex = xSemaphoreCreateMutex();
        configASSERT(log_mutex != NULL);
    }
}

uint32_t app_log_dropped(void)
{
    return log_dropped;
}

int vprintf(const char *fmt, va_list ap)
{
    int n;
    if (!log_lock()) {
        return -1;
    }
    n = vsnprintf(log_buf, sizeof log_buf, fmt, ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof log_buf ? (size_t)n : sizeof log_buf - 1U;
        log_emit(log_buf, len);
    }
    log_unlock();
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s)
{
    size_t len = strlen(s);
    if (!log_lock()) {
        return EOF;
    }
    log_emit(s, len);
    log_emit("\n", 1);
    log_unlock();
    return (int)len + 1;
}

int putchar(int c)
{
    char ch = (char)c;
    if (!log_lock()) {
        return EOF;
    }
    log_emit(&ch, 1);
    log_unlock();
    return (unsigned char)c;
}

/* ------------------------------------------------------------------------
 * Panic path: interrupts off, direct register polling, no HAL, no mutex,
 * no libc formatting.  Safe from hooks, asserts and fault handlers.
 * --------------------------------------------------------------------- */
static void panic_putc(char c)
{
    while ((USART2->SR & USART_SR_TXE) == 0U) {
    }
    USART2->DR = (uint8_t)c;
}

static void panic_puts(const char *s)
{
    while (s != NULL && *s != '\0') {
        if (*s == '\n') {
            panic_putc('\r');
        }
        panic_putc(*s++);
    }
}

static void panic_putu(uint32_t v)
{
    char tmp[11];
    int i = 0;
    do {
        tmp[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U && i < (int)sizeof tmp);
    while (i > 0) {
        panic_putc(tmp[--i]);
    }
}

void app_panic(const char *what, const char *detail, uint32_t value)
{
    __disable_irq();
    panic_puts("\n*** PANIC: ");
    panic_puts(what);
    if (detail != NULL) {
        panic_puts(" [");
        panic_puts(detail);
        panic_puts("]");
    }
    panic_puts(" ");
    panic_putu(value);
    panic_puts(" - halted\n");
    while ((USART2->SR & USART_SR_TC) == 0U) {
    }
    for (;;) {
        /* halted: pause the debugger here to inspect the call stack */
    }
}
