/*
 * echo.h - TCP echo server (BSD sockets), port APP_ECHO_PORT, one client.
 */
#ifndef ECHO_H
#define ECHO_H

#include <stdbool.h>
#include <stdint.h>

/* Creates the echo task. The task owns the listening socket and the client
 * socket; no other task may touch them. */
void echo_start(void);

/* Queues a "button pressed <count>" message for the connected client.
 * Safe from any task (not from an ISR); never blocks. Returns false if the
 * queue is full. The echo task sends it within one select() period. */
bool echo_post_button(uint32_t count);

typedef struct {
    uint32_t accepted;      /* clients accepted                     */
    uint32_t rejected;      /* refused because one was connected    */
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t button_sent;   /* button messages sent to a client     */
    uint32_t button_nocl;   /* ... discarded: no client connected   */
    uint32_t stack_free;    /* echo task stack high-water mark, words */
} echo_stats_t;

void echo_get_stats(echo_stats_t *out);

#endif /* ECHO_H */
