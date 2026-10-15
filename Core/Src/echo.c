/*
 * echo.c - TCP echo server on port APP_ECHO_PORT using the BSD socket API.
 *
 * One task owns both sockets (listener + the single client) and waits on
 * them with select() and a short timeout, so that it can also service a
 * FreeRTOS queue between wake-ups: the button task posts press counts to
 * that queue and this task sends "button pressed <count>\r\n".
 *
 * Behaviour
 *   - one client at a time; a second connection is accepted, told
 *     "busy" and closed immediately (it would otherwise sit silently in
 *     the listen backlog);
 *   - every received byte is sent back; recv() == 0 (client closed) or an
 *     error closes the socket cleanly and the server goes back to waiting;
 *   - link loss: the connection is kept. TCP retransmits when the cable
 *     is back. If the peer has really gone, TCP keep-alive (idle 10 s,
 *     6 probes 5 s apart, ~40 s in total) drops the dead connection so a
 *     new client can connect - no MCU reset needed;
 *   - send() cannot block forever: SO_SNDTIMEO drops a client that stops
 *     acknowledging data.
 */
#include "echo.h"
#include "app.h"
#include "app_config.h"
#include "app_log.h"
#include "net.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define ECHO_BUF_SIZE        512
#define ECHO_SELECT_MS       50      /* also the worst-case queue latency */
#define ECHO_SEND_TIMEOUT_S  5
#define ECHO_KEEPIDLE_S      10
#define ECHO_KEEPINTVL_S     5
#define ECHO_KEEPCNT         6
#define ECHO_BUTTON_QUEUE_LEN 8

static uint8_t echo_buf[ECHO_BUF_SIZE];   /* only the echo task uses it */
static echo_stats_t stats;
static QueueHandle_t button_queue;        /* uint32_t press counts */

void echo_get_stats(echo_stats_t *out)
{
    taskENTER_CRITICAL();
    *out = stats;
    taskEXIT_CRITICAL();
}

bool echo_post_button(uint32_t count)
{
    return button_queue != NULL && xQueueSend(button_queue, &count, 0) == pdTRUE;
}

/* Sends everything or returns -1 (error or SO_SNDTIMEO expired). */
static int send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0U) {
        int n = send(fd, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
        stats.tx_bytes += (uint32_t)n;
    }
    return 0;
}

static int open_listener(void)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(APP_ECHO_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void configure_client(int fd)
{
    struct timeval tv = { ECHO_SEND_TIMEOUT_S, 0 };
    int on = 1;
    int idle = ECHO_KEEPIDLE_S, intvl = ECHO_KEEPINTVL_S, cnt = ECHO_KEEPCNT;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);  /* echo keystrokes at once */
}

static void drop_client(int *cfd, const char *why)
{
    if (*cfd >= 0) {
        close(*cfd);
        *cfd = -1;
        app_status_client(false);
        printf("[echo] client closed (%s)\n", why);
    }
}

static void handle_accept(int lfd, int *cfd)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    char ip[16];
    int nfd = accept(lfd, (struct sockaddr *)&peer, &plen);
    if (nfd < 0) {
        return;
    }
    (void)inet_ntoa_r(peer.sin_addr, ip, sizeof ip);
    if (*cfd >= 0) {
        static const char busy[] = "busy: one client at a time\r\n";
        (void)send(nfd, busy, sizeof busy - 1U, MSG_DONTWAIT);
        close(nfd);
        stats.rejected++;
        printf("[echo] rejected %s:%u (busy)\n", ip, (unsigned)ntohs(peer.sin_port));
        return;
    }
    configure_client(nfd);
    *cfd = nfd;
    stats.accepted++;
    app_status_client(true);
    printf("[echo] client %s:%u connected\n", ip, (unsigned)ntohs(peer.sin_port));
}

static void handle_client(int *cfd)
{
    int n = recv(*cfd, echo_buf, sizeof echo_buf, 0);
    if (n > 0) {
        stats.rx_bytes += (uint32_t)n;
        if (send_all(*cfd, echo_buf, (size_t)n) < 0) {
            drop_client(cfd, "send failed or timed out");
        }
    } else if (n == 0) {
        drop_client(cfd, "disconnected by peer");
    } else {
        drop_client(cfd, errno == ECONNRESET ? "connection reset" :
                         errno == ENOTCONN ? "connection lost (keep-alive/abort)" :
                         "recv error");
    }
}

/* Drains the button queue. Runs in the echo task, the only task that
 * touches the client socket, so it cannot collide with the echo traffic. */
static void service_button_queue(int *cfd)
{
    uint32_t count;
    while (xQueueReceive(button_queue, &count, 0) == pdTRUE) {
        char msg[40];
        int len;
        if (*cfd < 0) {
            stats.button_nocl++;
            continue;                       /* nobody connected: discard */
        }
        len = snprintf(msg, sizeof msg, "button pressed %lu\r\n", (unsigned long)count);
        if (send_all(*cfd, msg, (size_t)len) < 0) {
            drop_client(cfd, "send failed or timed out");
        } else {
            stats.button_sent++;
        }
    }
}

static void echo_task(void *arg)
{
    int lfd = -1;
    int cfd = -1;
    (void)arg;

    while (!net_is_up()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    for (;;) {
        fd_set rfds;
        struct timeval tv = { 0, ECHO_SELECT_MS * 1000 };
        int maxfd, n;

        if (lfd < 0) {
            lfd = open_listener();
            if (lfd < 0) {
                printf("[echo] cannot open listener (errno %d), retrying\n", errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            printf("[echo] listening on TCP port %u\n", (unsigned)APP_ECHO_PORT);
        }

        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        maxfd = lfd;
        if (cfd >= 0) {
            FD_SET(cfd, &rfds);
            if (cfd > maxfd) {
                maxfd = cfd;
            }
        }
        n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            printf("[echo] select error (errno %d)\n", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (n > 0) {
            /* Client first: a readable client may be closed in here, which
             * frees the slot before a pending accept is looked at. */
            if (cfd >= 0 && FD_ISSET(cfd, &rfds)) {
                handle_client(&cfd);
            }
            if (FD_ISSET(lfd, &rfds)) {
                handle_accept(lfd, &cfd);
            }
        }
        service_button_queue(&cfd);

        stats.stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    }
}

void echo_start(void)
{
    button_queue = xQueueCreate(ECHO_BUTTON_QUEUE_LEN, sizeof(uint32_t));
    configASSERT(button_queue != NULL);
    if (xTaskCreate(echo_task, "echo", APP_STACK_ECHO, NULL,
                    APP_PRIO_ECHO, NULL) != pdPASS) {
        app_panic("xTaskCreate", "echo", 0);
    }
}
