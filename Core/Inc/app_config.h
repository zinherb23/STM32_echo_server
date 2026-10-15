/*
 * app_config.h - task priorities, stack sizes and board constants
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ---- priorities ------------------------------------------------------- */
#define APP_PRIO_ENC_RX        5
#define APP_PRIO_TCPIP         4
#define APP_PRIO_ECHO          3
#define APP_PRIO_BUTTON        2

/* ---- stacks (words) --------------------------------------------------- */
#define APP_STACK_ENC_RX       512   /* 2 KB: printf (vsnprintf) + netifapi */
#define APP_STACK_TCPIP        1024  /* 4 KB: TCP input/output, ARP, ICMP   */
#define APP_STACK_ECHO         768   /* 3 KB: sockets API, select(), fd_set */
#define APP_STACK_BUTTON       384   /* 1.5 KB: printf only                 */

/* ---- network ---------------------------------------------------------- */
#define APP_MAC_ADDR           { 0x02, 0x23, 0x45, 0x67, 0x89, 0xAB }
#define APP_IP_ADDR            192, 168, 1, 200
#define APP_NETMASK            255, 255, 255, 0
#define APP_GATEWAY            192, 168, 1, 1
#define APP_ECHO_PORT          7       /* TCP echo server */

/* ---- misc ------------------------------------------------------------- */
#define APP_HEARTBEAT_MS       10000U  /* status line period                */

#endif /* APP_CONFIG_H */
