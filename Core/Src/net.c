/*
 * net.c - ENC28J60 bring-up
 */
#include "net.h"
#include "app.h"
#include "app_config.h"
#include "app_log.h"
#include "enc.h"
#include "ethernetif.h"
#include "echo.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "lwip/tcpip.h"
#include "lwip/netifapi.h"
#include "lwip/ip4_addr.h"
#include <stdio.h>

extern SPI_HandleTypeDef hspi3;

/* Expands the comma-separated APP_IP_ADDR etc. before IP4_ADDR sees them. */
#define IP4_ADDR_SET(addr, ...)  IP4_ADDR(addr, __VA_ARGS__)

#define RX_WAIT_MS   100U   /* safety-net timeout for ulTaskNotifyTake */
#define RX_BURST     16U    /* frames per wake-up before re-arming INT */

static const uint8_t net_mac[6] = APP_MAC_ADDR;
static struct netif enc_netif;
static TaskHandle_t rx_task;
static volatile bool netif_ready;
static volatile bool link_state;
static bool link_known;
static uint32_t wake_irq;    /* RX task wake-ups caused by the ENC INT pin */
static uint32_t wake_poll;   /* wake-ups caused by the 100 ms safety-net timeout */

bool net_is_up(void)       { return netif_ready; }
bool net_link_is_up(void)  { return link_state; }

void net_enc_int_from_isr(void)
{
    BaseType_t woken = pdFALSE;
    if (rx_task != NULL) {
        vTaskNotifyGiveFromISR(rx_task, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

void net_request_service(void)
{
    if (rx_task != NULL) {
        xTaskNotifyGive(rx_task);
    }
}

/* Bring-up aid, printed after a failed init: bad MAC-register reads for
 * each SPI3 prescaler and SCK/MOSI pin speed (see enc28j60_spi_sweep). */
static void spi_sweep_report(void)
{
    static const char *const rate[ENC_SWEEP_RATES] = {
        "/4  10.5 MHz", "/8  5.25 MHz", "/16 2.63 MHz", "/32 1.31 MHz"
    };
    static enc_sweep_t sw;      /* static: keep it off the task stack */
    enc28j60_spi_sweep(net_mac, &sw);
    printf("[enc] SPI sweep (bad MAC reads out of %u)%s\n", (unsigned)sw.reads,
           sw.spi_failed ? " - aborted, SPI HAL error" : "");
    printf("[enc]   as configured, MAC only: %u bad, per byte %u %u %u %u %u %u, first: byte %u read %02X\n",
           (unsigned)sw.as_configured,
           (unsigned)sw.per_byte[0], (unsigned)sw.per_byte[1], (unsigned)sw.per_byte[2],
           (unsigned)sw.per_byte[3], (unsigned)sw.per_byte[4], (unsigned)sw.per_byte[5],
           (unsigned)sw.first_idx, sw.first_got);
    printf("[enc]   as configured, with PHY: %u bad, per byte %u %u %u %u %u %u, first: byte %u read %02X\n",
           (unsigned)sw.as_configured_phy,
           (unsigned)sw.per_byte_phy[0], (unsigned)sw.per_byte_phy[1], (unsigned)sw.per_byte_phy[2],
           (unsigned)sw.per_byte_phy[3], (unsigned)sw.per_byte_phy[4], (unsigned)sw.per_byte_phy[5],
           (unsigned)sw.first_idx_phy, sw.first_got_phy);
    printf("[enc]   prescaler        Low  Medium  High  VeryHigh   (SCK/MOSI pin speed)\n");
    for (unsigned r = 0; r < ENC_SWEEP_RATES; ++r) {
        printf("[enc]   %s   %4u  %6u  %4u  %8u\n", rate[r],
               (unsigned)sw.errors[r][0], (unsigned)sw.errors[r][1],
               (unsigned)sw.errors[r][2], (unsigned)sw.errors[r][3]);
    }
}

/* Initialise the chip, retrying with back-off until it answers correctly. */
static void enc_bring_up(void)
{
    uint32_t attempt = 0;
    for (;;) {
        enc_status_t st = enc28j60_init(&hspi3, net_mac);
        attempt++;
        if (st == ENC_OK) {
            break;
        }
        {
            enc_diag_t d;
            enc28j60_get_diag(&d);
            printf("[enc] init failed, attempt %lu: %s (status %d, EREVID 0x%02X)\n",
                   (unsigned long)attempt, enc28j60_step_name(d.step), (int)st,
                   enc28j60_revision());
            if (d.step == ENC_STEP_MAC_VERIFY) {
                printf("[enc]   MAC byte %u: wrote %02X, read %02X; all six read as "
                       "%02X:%02X:%02X:%02X:%02X:%02X\n",
                       (unsigned)d.index, d.expected, d.got,
                       d.mac_dump[0], d.mac_dump[1], d.mac_dump[2],
                       d.mac_dump[3], d.mac_dump[4], d.mac_dump[5]);
            }
            if (d.eth_tests != 0U) {
                printf("[enc]   SPI self-test: ETH reg %u/%u bad (wrote %02X read %02X), "
                       "MAC reg %u/%u bad (wrote %02X read %02X)\n",
                       (unsigned)d.eth_errors, (unsigned)d.eth_tests, d.eth_wrote, d.eth_read,
                       (unsigned)d.mac_errors, (unsigned)d.mac_tests, d.mac_wrote, d.mac_read);
            }
        }
        if (attempt == 1U || (attempt % 10U) == 0U) {
            spi_sweep_report();
        }
        vTaskDelay(pdMS_TO_TICKS(attempt < 10U ? 100U * attempt : 1000U));
    }
    {
        uint8_t mac[6];
        uint8_t rev = enc28j60_revision();
        printf("[enc] EREVID 0x%02X (rev %s)\n", rev, enc28j60_revision_name(rev));
        if (enc28j60_read_mac(mac) == ENC_OK) {
            printf("[enc] MAC %02X:%02X:%02X:%02X:%02X:%02X (read back from MAADR)\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }
}

static void report_link(bool up)
{
    if (link_known && up == link_state) {
        return;
    }
    link_known = true;
    link_state = up;
    if (netif_ready) {
        if (up) {
            netifapi_netif_set_link_up(&enc_netif);
        } else {
            netifapi_netif_set_link_down(&enc_netif);
        }
    }
    printf("[net] link %s\n", up ? "up" : "down");
    app_status_link(up);
}

static void recover(void)
{
    enc_stats_t s;
    printf("[enc] SPI/chip failure detected, re-initialising ENC28J60\n");
    report_link(false);
    enc_bring_up();
    enc28j60_get_stats(&s);
    printf("[enc] recovered (re-init #%lu, SPI errors %lu)\n",
           (unsigned long)s.reinits, (unsigned long)s.spi_errors);
    report_link(enc28j60_link_up());
}

static void heartbeat(void)
{
    enc_stats_t s;
    echo_stats_t e;
    enc28j60_get_stats(&s);
    echo_get_stats(&e);
    printf("[hb] up %lus heap %u (min %u) | rx %lu drop %lu bad %lu ovf %lu rst %lu | "
           "tx %lu retry %lu fail %lu | reinit %lu | wake irq %lu poll %lu\n",
           (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ),
           (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize(),
           (unsigned long)s.rx_frames, (unsigned long)s.rx_dropped,
           (unsigned long)s.rx_bad, (unsigned long)s.rx_overflows,
           (unsigned long)s.rx_resets, (unsigned long)s.tx_frames,
           (unsigned long)s.tx_retries, (unsigned long)s.tx_failed,
           (unsigned long)s.reinits,
           (unsigned long)wake_irq, (unsigned long)wake_poll);
    printf("[hb]   echo: clients %lu rejected %lu, bytes in %lu out %lu, button sent %lu "
           "no-client %lu | stack free (words): enc_rx %lu echo %lu\n",
           (unsigned long)e.accepted, (unsigned long)e.rejected,
           (unsigned long)e.rx_bytes, (unsigned long)e.tx_bytes,
           (unsigned long)e.button_sent, (unsigned long)e.button_nocl,
           (unsigned long)uxTaskGetStackHighWaterMark(NULL),
           (unsigned long)e.stack_free);
}

static void tcpip_ready(void *arg)
{
    xSemaphoreGive((SemaphoreHandle_t)arg);   /* runs in tcpip_thread */
}

static void lwip_bring_up(void)
{
    ip4_addr_t ip, mask, gw;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    configASSERT(done != NULL);
    tcpip_init(tcpip_ready, done);
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);

    IP4_ADDR_SET(&ip, APP_IP_ADDR);
    IP4_ADDR_SET(&mask, APP_NETMASK);
    IP4_ADDR_SET(&gw, APP_GATEWAY);
    if (netifapi_netif_add(&enc_netif, &ip, &mask, &gw, NULL,
                           ethernetif_init, tcpip_input) != ERR_OK) {
        app_panic("netifapi_netif_add", NULL, 0);
    }
    netifapi_netif_set_default(&enc_netif);
    netifapi_netif_set_up(&enc_netif);
    netif_ready = true;
    printf("[net] lwIP up: %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u\n",
           ip4_addr1(&ip), ip4_addr2(&ip), ip4_addr3(&ip), ip4_addr4(&ip),
           ip4_addr1(&mask), ip4_addr2(&mask), ip4_addr3(&mask), ip4_addr4(&mask),
           ip4_addr1(&gw), ip4_addr2(&gw), ip4_addr3(&gw), ip4_addr4(&gw));
}

static void enc_rx_task(void *arg)
{
    TickType_t last_hb;
    (void)arg;

    enc_bring_up();
    lwip_bring_up();
    report_link(enc28j60_link_up());
    last_hb = xTaskGetTickCount();

    for (;;) {
        uint32_t woke = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RX_WAIT_MS));
        uint8_t eir;
        int frames;

        if (woke != 0U) {
            wake_irq++;
        } else {
            wake_poll++;
        }
        if (!enc28j60_ready()) {
            recover();
            continue;
        }
        eir = enc28j60_irq_begin();                    /* INTIE off, read EIR */
        frames = ethernetif_rx(&enc_netif, RX_BURST);  /* drain by EPKTCNT */
        if (frames < 0 || !enc28j60_ready()) {
            recover();
            continue;
        }
        enc28j60_irq_end(eir);                         /* RXERIF clear, INTIE on */

        if ((eir & ENC_EIR_LINKIF) != 0U || woke == 0U) {
            report_link(enc28j60_link_up());
        }
        if (woke == 0U && !enc28j60_check_alive()) {
            recover();
            continue;
        }
        if (frames >= (int)RX_BURST) {
            xTaskNotifyGive(xTaskGetCurrentTaskHandle());   /* more pending: go again */
        }
        if ((TickType_t)(xTaskGetTickCount() - last_hb) >= pdMS_TO_TICKS(APP_HEARTBEAT_MS)) {
            last_hb = xTaskGetTickCount();
            heartbeat();
        }
    }
}

void net_start(void)
{
    if (xTaskCreate(enc_rx_task, "enc_rx", APP_STACK_ENC_RX, NULL,
                    APP_PRIO_ENC_RX, &rx_task) != pdPASS) {
        app_panic("xTaskCreate", "enc_rx", 0);
    }
}
