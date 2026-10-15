/*
 * app_log.h
 */
#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

void app_log_init(void);                     /* call once, before any task runs */
uint32_t app_log_dropped(void);              /* printf calls rejected from ISR  */

void app_panic(const char *what, const char *detail, uint32_t value)
    __attribute__((noreturn));

#endif /* APP_LOG_H */
