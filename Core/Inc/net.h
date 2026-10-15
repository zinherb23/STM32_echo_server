/*
 * net.h - network bring-up and the ENC28J60 RX task.
 */
#ifndef NET_H
#define NET_H

#include <stdbool.h>

/* Creates the ENC RX task. That task initialises the ENC28J60, starts lwIP
 * (tcpip_init), adds the netif and then services the chip. */
void net_start(void);

/* true once the netif is added and up (IP configured). */
bool net_is_up(void);

/* Current link state as last reported to lwIP. */
bool net_link_is_up(void);

/* From the ENC INT (PA0, EXTI0) ISR: wake the RX task. */
void net_enc_int_from_isr(void);

/* From task context: wake the RX task (e.g. after a TX SPI failure). */
void net_request_service(void);

#endif /* NET_H */
