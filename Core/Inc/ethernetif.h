/*
 * ethernetif.h - lwIP netif glue for the ENC28J60 (NO_SYS = 0).
 */
#ifndef ETHERNETIF_H
#define ETHERNETIF_H

#include "lwip/netif.h"

/* netif init callback for netifapi_netif_add(..., ethernetif_init, tcpip_input).
 * Only fills in the netif; the chip itself is initialised by net.c. */
err_t ethernetif_init(struct netif *netif);

/* Reads up to max_frames pending frames from the ENC28J60 and hands them to
 * netif->input (tcpip_input). Returns the number of frames read (including
 * dropped ones), or -1 on an SPI/chip failure. Task context only. */
int ethernetif_rx(struct netif *netif, unsigned max_frames);

#endif /* ETHERNETIF_H */
