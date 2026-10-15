/*
 * ethernetif.c - lwIP netif glue for the ENC28J60
 */
#include "ethernetif.h"
#include "enc.h"
#include "net.h"
#include "app_config.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include <stdbool.h>
#include <string.h>

static err_t linkoutput(struct netif *netif, struct pbuf *p)
{
    enc_cseg_t segs[ENC28J60_MAX_SEGS];
    struct pbuf *flat = NULL;
    struct pbuf *q;
    unsigned n = 0;
    enc_status_t st;
    (void)netif;

    if (p->tot_len > ENC28J60_MAX_FRAME) {
        return ERR_BUF;
    }
    bool too_long = false;
    for (q = p; q != NULL; q = q->next) {
        if (q->len != 0U) {
            if (n == ENC28J60_MAX_SEGS) {
                too_long = true;
                break;
            }
            segs[n].data = (const uint8_t *)q->payload;
            segs[n].len = q->len;
            n++;
        }
        if (q->len == q->tot_len) {
            break;      /* last pbuf of this packet */
        }
    }
    if (too_long) {
        /* Unusually long chain: flatten it into one PBUF_RAM pbuf. */
        flat = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
        if (flat == NULL) {
            return ERR_MEM;
        }
        segs[0].data = (const uint8_t *)flat->payload;
        segs[0].len = flat->len;
        n = 1;
    }

    st = enc28j60_send(segs, n, p->tot_len);
    if (flat != NULL) {
        pbuf_free(flat);
    }
    switch (st) {
    case ENC_OK:
        return ERR_OK;
    case ENC_ERR_IO:
        net_request_service();   /* let the RX task run recovery now */
        return ERR_IF;
    case ENC_ERR_NOT_READY:
        return ERR_IF;           /* recovering or link reset: TCP retransmits later */
    default:
        return ERR_IF;
    }
}

err_t ethernetif_init(struct netif *netif)
{
    static const uint8_t mac[6] = APP_MAC_ADDR;
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, mac, sizeof mac);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = linkoutput;
    return ERR_OK;
}

/* Driver callback (SPI mutex held): provide buffers for a frame of len bytes. */
static unsigned rx_alloc(void *ctx, uint16_t len, enc_seg_t *segs, unsigned max_segs)
{
    struct pbuf **out = (struct pbuf **)ctx;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    struct pbuf *q;
    unsigned n = 0;

    *out = NULL;
    if (p == NULL) {
        return 0;               /* pool exhausted: frame is dropped */
    }
    for (q = p; q != NULL; q = q->next) {
        if (n == max_segs) {
            pbuf_free(p);
            return 0;
        }
        segs[n].data = (uint8_t *)q->payload;
        segs[n].len = q->len;
        n++;
    }
    *out = p;
    return n;
}

int ethernetif_rx(struct netif *netif, unsigned max_frames)
{
    unsigned frames = 0;
    while (frames < max_frames && enc28j60_packet_count() != 0U) {
        struct pbuf *p = NULL;
        int r = enc28j60_receive(rx_alloc, &p);
        if (r < 0) {
            if (p != NULL) {
                pbuf_free(p);
            }
            return -1;
        }
        frames++;
        if (r > 0 && p != NULL) {
            /* tcpip_input owns p on success; free it if the mbox is full. */
            if (netif->input(p, netif) != ERR_OK) {
                pbuf_free(p);
            }
        } else if (p != NULL) {
            pbuf_free(p);
        }
    }
    return (int)frames;
}
