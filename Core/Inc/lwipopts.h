/*
 * lwipopts.h - lwIP 2.2.1, NO_SYS = 0 (FreeRTOS), BSD sockets, IPv4/TCP only.
 * Target: STM32F401RE (96 KB RAM), ENC28J60 10BASE-T half duplex.
 *
 * Static RAM used by lwIP with these settings (approx.):
 *   PBUF pool   10 x ~1.55 KB   = 15.5 KB   (RX frames, one frame per pbuf)
 *   MEM_SIZE                        8.0 KB   (TX pbufs: TCP send data, ARP)
 *   memp pools (PCBs, segs, msgs)  ~2.5 KB
 * Task stacks, mboxes and semaphores live in the FreeRTOS heap.
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#include "app_config.h"

/* ---------------------------------------------------------------------
 * OS integration
 * ------------------------------------------------------------------ */
#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1   /* SYS_ARCH_PROTECT = FreeRTOS critical section */
#define LWIP_TIMERS                     1

/* Core locking: application threads call into the stack by taking the
 * core mutex instead of posting a message and waiting for tcpip_thread.
 * Faster socket calls; netif API calls also just lock and run. */
#define LWIP_TCPIP_CORE_LOCKING         1
#define LWIP_TCPIP_CORE_LOCKING_INPUT   0   /* RX frames still go through the
                                               tcpip mbox (tcpip_input), so the
                                               ENC RX task never runs TCP code */

/* Development: assert that stack functions are only called with the core
 * lock held (or from tcpip_thread). Implemented in the FreeRTOS sys_arch.c. */
#define LWIP_FREERTOS_CHECK_CORE_LOCKING 1
void sys_lock_tcpip_core(void);
void sys_unlock_tcpip_core(void);
void sys_check_core_locking(void);
void sys_mark_tcpip_thread(void);
#define LOCK_TCPIP_CORE()               sys_lock_tcpip_core()
#define UNLOCK_TCPIP_CORE()             sys_unlock_tcpip_core()
#define LWIP_ASSERT_CORE_LOCKED()       sys_check_core_locking()
#define LWIP_MARK_TCPIP_THREAD()        sys_mark_tcpip_thread()

/* sys_thread_new() stack size given in words, like xTaskCreate(). */
#define LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS 1

#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE          APP_STACK_TCPIP
#define TCPIP_THREAD_PRIO               APP_PRIO_TCPIP
#define TCPIP_MBOX_SIZE                 16  /* >= PBUF_POOL_SIZE: every RX pbuf can queue */
#define DEFAULT_TCP_RECVMBOX_SIZE       8   /* per socket: pbufs waiting for recv() */
#define DEFAULT_ACCEPTMBOX_SIZE         2
#define DEFAULT_UDP_RECVMBOX_SIZE       2
#define DEFAULT_RAW_RECVMBOX_SIZE       2

/* ---------------------------------------------------------------------
 * APIs
 * ------------------------------------------------------------------ */
#define LWIP_NETCONN                    1   /* required by sockets */
#define LWIP_SOCKET                     1
#define LWIP_NETIF_API                  1   /* netifapi_netif_add / set_link_up */
#define LWIP_COMPAT_SOCKETS             1   /* socket(), bind(), select() ... names */
#define LWIP_POSIX_SOCKETS_IO_NAMES     1   /* close(), read(), write() */
#define LWIP_SOCKET_SELECT              1   /* echo task waits with select() */
#define LWIP_SOCKET_POLL                0
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1   /* send() cannot block forever on a dead link */
#define LWIP_TCP_KEEPALIVE              1   /* TCP_KEEPIDLE/INTVL/CNT socket options */
#define LWIP_NETCONN_FULLDUPLEX         0   /* one task owns each socket */
#define LWIP_NETCONN_SEM_PER_THREAD     0
#define LWIP_TIMEVAL_PRIVATE            0   /* use newlib's struct timeval (cc.h) */
#define LWIP_DNS                        0
#define LWIP_RAW                        0
#define LWIP_UDP                        0
#define LWIP_DHCP                       0
#define LWIP_AUTOIP                     0
#define LWIP_ACD                        0
#define LWIP_IGMP                       0
#define LWIP_IPV6                       0
#define LWIP_ALTCP                      0

/* ---------------------------------------------------------------------
 * Network interface
 * ------------------------------------------------------------------ */
#define LWIP_IPV4                       1
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_SINGLE_NETIF               1
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_HOSTNAME             0
#define ETH_PAD_SIZE                    0
#define ARP_TABLE_SIZE                  4
#define ARP_QUEUEING                    0
#define IP_FORWARD                      0
#define IP_REASSEMBLY                   0   /* no fragmented traffic expected */
#define IP_FRAG                         0   /* we never send > MTU */

/* ---------------------------------------------------------------------
 * Memory
 * ------------------------------------------------------------------ */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (8 * 1024)
#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_USE_POOLS                   0

/* One Ethernet frame (1514 bytes) fits one pool pbuf: the ENC driver then
 * reads each frame with a single RBM transfer. */
#define PBUF_POOL_SIZE                  10
#define PBUF_POOL_BUFSIZE               LWIP_MEM_ALIGN_SIZE(1514 + PBUF_LINK_ENCAPSULATION_HLEN)

#define MEMP_NUM_PBUF                   8
#define MEMP_NUM_NETCONN                4   /* listen + client + one being rejected + spare */
#define MEMP_NUM_NETBUF                 2
#define MEMP_NUM_TCP_PCB                4   /* incl. PCBs in TIME_WAIT (oldest are recycled) */
#define MEMP_NUM_TCP_PCB_LISTEN         1
#define MEMP_NUM_TCP_SEG                16  /* >= TCP_SND_QUEUELEN */
#define MEMP_NUM_TCPIP_MSG_INPKT        16  /* one per RX frame in the tcpip mbox */
#define MEMP_NUM_TCPIP_MSG_API          8

/* ---------------------------------------------------------------------
 * TCP
 * ------------------------------------------------------------------ */
#define LWIP_TCP                        1
#define TCP_MSS                         1460
#define TCP_WND                         (2 * TCP_MSS)  /* 2920 B: fits in the RX pool */
#define TCP_SND_BUF                     (2 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define TCP_QUEUE_OOSEQ                 0   /* drop out-of-order segments: saves pbufs */
#define TCP_LISTEN_BACKLOG              1
#define TCP_DEFAULT_LISTEN_BACKLOG      1
#define LWIP_TCP_SACK_OUT               0
#define LWIP_WND_SCALE                  0

/* ---------------------------------------------------------------------
 * Checksums (software), statistics (LWIP_DEBUG deliberately left undefined:
 * lwIP tests it with #ifdef)
 * ------------------------------------------------------------------ */
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0

#endif /* LWIPOPTS_H */
