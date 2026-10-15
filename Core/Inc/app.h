/*
 * app.h - application entry point and status LED.
 */
#ifndef APP_H
#define APP_H

#include <stdbool.h>

/* Creates the tasks and starts the FreeRTOS scheduler. Never returns.
 * Called from main() USER CODE 2. */
void app_start(void) __attribute__((noreturn));

/* LD2 (PA5) is on when the Ethernet link is up AND a TCP client is
 * connected. Task context only. */
void app_status_link(bool up);
void app_status_client(bool connected);

#endif /* APP_H */
