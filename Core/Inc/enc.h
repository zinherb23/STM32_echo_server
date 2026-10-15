/*
 * enc.h - ENC28J60 driver
 */
#ifndef ENC_H
#define ENC_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

#define ENC28J60_MAX_FRAME   1514U   /* without FCS */
#define ENC28J60_MAX_SEGS    8U

/* EIR bits returned by enc28j60_irq_begin() */
#define ENC_EIR_RXERIF   0x01U
#define ENC_EIR_TXERIF   0x02U
#define ENC_EIR_TXIF     0x08U
#define ENC_EIR_LINKIF   0x10U
#define ENC_EIR_PKTIF    0x40U

typedef enum {
    ENC_OK = 0,
    ENC_ERR_ARG,
    ENC_ERR_IO,          /* SPI transfer failed (HAL error/timeout)        */
    ENC_ERR_CHIP,        /* chip absent, not ready or unsupported revision */
    ENC_ERR_TX,          /* frame not sent after all retries               */
    ENC_ERR_NOT_READY    /* driver down / recovering                       */
} enc_status_t;

typedef struct {
    uint8_t *data;
    uint16_t len;
} enc_seg_t;

typedef struct {
    const uint8_t *data;
    uint16_t len;
} enc_cseg_t;

/* Called with the frame length; fills segs (total length == len) and
 * returns the number of segments, or 0 to drop the frame. Runs with the
 * SPI mutex held: must not block or call back into the driver. */
typedef unsigned (*enc_rx_alloc_fn)(void *ctx, uint16_t len,
                                    enc_seg_t *segs, unsigned max_segs);

typedef struct {
    uint32_t rx_frames;
    uint32_t rx_dropped;       /* no buffer from alloc callback            */
    uint32_t rx_bad;           /* CRC/length error, oversize               */
    uint32_t rx_resets;        /* corrupt next-packet pointer              */
    uint32_t rx_overflows;     /* RXERIF events                            */
    uint32_t tx_frames;
    uint32_t tx_retries;       /* late collision / TXERIF / stuck TXRTS    */
    uint32_t tx_failed;
    uint32_t spi_errors;
    uint32_t reinits;
} enc_stats_t;

/* Why the last enc28j60_init() failed, plus the result of a register
 * write/read-back self-test run after a revision/MAC-verify failure. */
typedef enum {
    ENC_STEP_NONE = 0,
    ENC_STEP_SPI,
    ENC_STEP_CLKRDY,
    ENC_STEP_REVISION,
    ENC_STEP_MAC_VERIFY
} enc_init_step_t;

typedef struct {
    uint8_t  step;                 /* enc_init_step_t */
    uint8_t  index;                /* MAC byte index for ENC_STEP_MAC_VERIFY */
    uint8_t  expected, got;
    uint16_t eth_tests, eth_errors;   /* ETH register write/read-back */
    uint8_t  eth_wrote, eth_read;     /* first mismatch */
    uint16_t mac_tests, mac_errors;   /* MAC register write/read-back */
    uint8_t  mac_wrote, mac_read;     /* first mismatch */
    uint8_t  mac_dump[6];             /* all six MAC bytes as read at a verify failure */
} enc_diag_t;

/* Bring-up aid: MAC-register write/read-back test repeated for SPI3
 * prescalers /4 /8 /16 /32 and SCK/MOSI pin speeds Low/Medium/High/VeryHigh
 * (both overridden at run time and restored afterwards). */
#define ENC_SWEEP_RATES    4U
#define ENC_SWEEP_SPEEDS   4U
#define ENC_SWEEP_REPEATS  16U
typedef struct {
    uint16_t reads;                    /* MAC reads per cell */
    uint16_t as_configured;            /* bad reads with the CubeMX settings */
    uint16_t per_byte[6];              /* ... split by MAC byte index */
    uint8_t  first_idx, first_got;     /* first bad read with the CubeMX settings */
    uint16_t as_configured_phy;        /* same, with PHY accesses before the read-back */
    uint16_t per_byte_phy[6];
    uint8_t  first_idx_phy, first_got_phy;
    uint16_t errors[ENC_SWEEP_RATES][ENC_SWEEP_SPEEDS];   /* with PHY accesses */
    bool     spi_failed;
} enc_sweep_t;

void enc28j60_spi_sweep(const uint8_t mac[6], enc_sweep_t *out);

void enc28j60_get_diag(enc_diag_t *out);
const char *enc28j60_step_name(uint8_t step);

/* (Re)initialise the chip: hardware reset on RESET pin, soft reset, checks
 * ESTAT/EREVID, programs buffers, MAC (half duplex), PHY and interrupts.
 * Re-initialises SPI3 too when recovering from an SPI error. */
enc_status_t enc28j60_init(SPI_HandleTypeDef *spi, const uint8_t mac[6]);

uint8_t  enc28j60_revision(void);                /* EREVID read at init   */
const char *enc28j60_revision_name(uint8_t rev);
enc_status_t enc28j60_read_mac(uint8_t mac[6]);  /* MAADR1..6 read back   */

/* true when init succeeded and no SPI error has been latched since.
 * A false value means enc28j60_init() must be called again. */
bool enc28j60_ready(void);

/* Liveness probe for the watchdog path: checks ECON1.RXEN is still set
 * (a chip that browned out/reset loses it) and latches a failure if not. */
bool enc28j60_check_alive(void);

/* PHSTAT2.LSTAT (real-time link state). */
bool enc28j60_link_up(void);

/* Interrupt handling (task context):
 *   eir = enc28j60_irq_begin();  clears EIE.INTIE (INT pin released),
 *                                reads EIR, clears LINKIF by reading PHIR
 *   ... drain with enc28j60_receive() until it returns 0 ...
 *   enc28j60_irq_end(eir);       clears RXERIF if set, sets EIE.INTIE again
 *                                (INT re-asserts if anything is still pending) */
uint8_t enc28j60_irq_begin(void);
void    enc28j60_irq_end(uint8_t eir);

uint8_t enc28j60_packet_count(void);           /* EPKTCNT */

/* Receives one frame (FCS stripped) into buffers provided by alloc.
 * Returns the frame length, 0 if none pending or the frame was dropped,
 * -1 on SPI failure. The ENC packet is always released (PKTDEC). */
int enc28j60_receive(enc_rx_alloc_fn alloc, void *ctx);

/* Sends one frame given as a list of segments (one WBM transfer), waits
 * for completion (TXIF/TXERIF, timeout) and retries on TX error or late
 * collision (DS80349C errata). */
enc_status_t enc28j60_send(const enc_cseg_t *segs, unsigned nsegs, uint16_t total);

void enc28j60_get_stats(enc_stats_t *out);

#endif /* ENC_H */
