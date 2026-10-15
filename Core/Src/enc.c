/*
 * enc.c - ENC28J60 driver for FreeRTOS
 */
#include "enc.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>


#if !defined(ENC_CS_Pin) || !defined(ENC_CS_GPIO_Port) || \
    !defined(ENC_RESET_Pin) || !defined(ENC_RESET_GPIO_Port)
#error "Set CubeMX user labels: PA4 = ENC_CS, PB0 = ENC_RESET (they generate the defines in main.h)"
#endif

/* Register addresses: bits 6:5 = bank, bit 7 = MAC/MII (needs dummy read) */
#define ERDPTL   0x00U
#define EWRPTL   0x02U
#define ETXSTL   0x04U
#define ETXNDL   0x06U
#define ERXSTL   0x08U
#define ERXNDL   0x0AU
#define ERXRDPTL 0x0CU
#define ERXFCON  0x38U
#define EPKTCNT  0x39U
#define MACON1   0xC0U
#define MACON3   0xC2U
#define MACON4   0xC3U
#define MABBIPG  0xC4U
#define MAIPGL   0xC6U
#define MAIPGH   0xC7U
#define MAMXFLL  0xCAU
#define MICMD    0xD2U
#define MIREGADR 0xD4U
#define MIWRL    0xD6U
#define MIRDL    0xD8U
#define MIRDH    0xD9U
#define MISTAT   0xEAU
#define EBSTSD   0x66U   /* bank 3 ETH register, free R/W (self-test seed) */
#define EREVID   0x72U
#define EIE      0x1BU
#define EIR      0x1CU
#define ESTAT    0x1DU
#define ECON2    0x1EU
#define ECON1    0x1FU

/* PHY registers */
#define PHCON1   0x00U
#define PHCON2   0x10U
#define PHSTAT2  0x11U
#define PHIE     0x12U
#define PHIR     0x13U

/* SPI opcodes */
#define RCR      0x00U
#define WCR      0x40U
#define BFS      0x80U
#define BFC      0xA0U
#define RBM      0x3AU
#define WBM      0x7AU
#define SRC      0xFFU

/* bits */
#define RXEN     0x04U   /* ECON1 */
#define TXRTS    0x08U
#define RXRST    0x40U
#define TXRST    0x80U
#define PKTDEC   0x40U   /* ECON2 */
#define AUTOINC  0x80U
#define CLKRDY   0x01U   /* ESTAT */
#define INTIE    0x80U   /* EIE */
#define PKTIE    0x40U
#define LINKIE   0x10U
#define RXERIE   0x01U
#define MIIRD    0x01U   /* MICMD */
#define MIBUSY   0x01U   /* MISTAT */
#define PGEIE    0x0002U /* PHIE */
#define PLNKIE   0x0010U
#define LSTAT    0x0400U /* PHSTAT2 */
#define HDLDIS   0x0100U /* PHCON2 */

#define RX_START 0x0000U
#define RX_END   0x19FFU /* Odd end, RX begins at zero (silicon errata). */
#define TX_START 0x1A00U /* Room for control byte, 1514-byte frame and TSV. */

#define SPI_TIMEOUT_MS     20U
#define MII_MAX_POLLS      2000U  /* MII op takes 10.24 us; one poll ~5 us    */
#define TX_ATTEMPTS        4U     /* 1 try + 3 retries                        */
#define TX_SPIN_POLLS      64U    /* ~1.5 ms of busy polling, then 1-tick sleeps */
#define TX_TIMEOUT_MS      50U    /* worst-case half-duplex backoff is ~52 ms  */

static SPI_HandleTypeDef *enc_spi;
static SemaphoreHandle_t enc_mutex;
static uint8_t bank;
static volatile uint8_t io_failed;
static volatile bool ready;
static bool spi_dirty;
static bool initialised_once;
static uint8_t revision;
static uint16_t next_packet;
static enc_stats_t stats;
static enc_diag_t diag;

/* ------------------------------------------------------------------------
 * low level (caller holds enc_mutex)
 * --------------------------------------------------------------------- */
static void lock(void)   { (void)xSemaphoreTake(enc_mutex, portMAX_DELAY); }
static void unlock(void) { (void)xSemaphoreGive(enc_mutex); }

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) + 1U);   /* +1: guarantee at least ms */
}

static void select_enc(void)   { HAL_GPIO_WritePin(ENC_CS_GPIO_Port, ENC_CS_Pin, GPIO_PIN_RESET); }
static void deselect_enc(void) { HAL_GPIO_WritePin(ENC_CS_GPIO_Port, ENC_CS_Pin, GPIO_PIN_SET); }

static void fail_io(void)
{
    if (!io_failed) {
        stats.spi_errors++;
    }
    io_failed = 1;
    ready = false;
    spi_dirty = true;
}

static uint8_t exchange(uint8_t value)
{
    uint8_t result = 0;
    if (!io_failed && HAL_SPI_TransmitReceive(enc_spi, &value, &result, 1,
                                             SPI_TIMEOUT_MS) != HAL_OK) {
        fail_io();
    }
    return result;
}

static void operation(uint8_t op, uint8_t reg, uint8_t value)
{
    select_enc();
    exchange((uint8_t)(op | (reg & 0x1FU)));
    exchange(value);
    deselect_enc();
}

static void set_bank(uint8_t reg)
{
    uint8_t wanted = (reg >> 5) & 3U;
    if ((reg & 0x1FU) < EIE && wanted != bank) {
        operation(BFC, ECON1, 3);
        operation(BFS, ECON1, wanted);
        bank = wanted;
    }
}

static uint8_t read_reg(uint8_t reg)
{
    uint8_t value;
    set_bank(reg);
    select_enc();
    exchange((uint8_t)(RCR | (reg & 0x1FU)));
    value = exchange(0);
    if (reg & 0x80U) {
        value = exchange(0);   /* MAC/MII registers shift out a dummy byte first */
    }
    deselect_enc();
    return value;
}

static void write_reg(uint8_t reg, uint8_t value)
{
    set_bank(reg);
    operation(WCR, reg, value);
}

static void write16(uint8_t reg, uint16_t value)
{
    write_reg(reg, (uint8_t)value);
    write_reg((uint8_t)(reg + 1U), (uint8_t)(value >> 8));
}

static void read_buffer(uint8_t *data, uint16_t length)
{
    if (length == 0U) {
        return;
    }
    select_enc();
    exchange(RBM);
    /* In two-line master mode HAL_SPI_Receive clocks its buffer out as dummy data. */
    memset(data, 0, length);
    if (!io_failed && HAL_SPI_Receive(enc_spi, data, length, SPI_TIMEOUT_MS) != HAL_OK) {
        fail_io();
    }
    deselect_enc();
}

static void mii_wait(void)
{
    unsigned polls = 0;
    while (!io_failed && (read_reg(MISTAT) & MIBUSY)) {
        if (++polls >= MII_MAX_POLLS) {
            fail_io();
        }
    }
}

static void write_phy(uint8_t reg, uint16_t value)
{
    write_reg(MIREGADR, reg);
    write16(MIWRL, value);          /* writing MIWRH starts the MII write */
    mii_wait();
}

static uint16_t read_phy(uint8_t reg)
{
    uint16_t value;
    write_reg(MIREGADR, reg);
    write_reg(MICMD, MIIRD);
    mii_wait();
    write_reg(MICMD, 0);
    value = read_reg(MIRDL);
    value |= (uint16_t)read_reg(MIRDH) << 8;
    return value;
}

static void reset_rx(void)
{
    uint8_t count;
    operation(BFC, ECON1, RXEN);
    operation(BFS, ECON1, RXRST);
    operation(BFC, ECON1, RXRST);
    count = read_reg(EPKTCNT);
    for (unsigned i = 0; i < count && !io_failed; ++i) {
        operation(BFS, ECON2, PKTDEC);
    }
    write16(ERXSTL, RX_START);
    write16(ERXNDL, RX_END);
    write16(ERXRDPTL, RX_END);
    next_packet = RX_START;
    operation(BFC, EIR, ENC_EIR_RXERIF);
    operation(BFS, ECON1, RXEN);
}

/* ------------------------------------------------------------------------
 * init
 * --------------------------------------------------------------------- */

/* Write/read-back test of one ETH register (16 SCK cycles per access) and
 * one MAC register (24 cycles per read, dummy byte first). Both are in
 * bank 3, so no bank switching happens inside the loop. Used only after a
 * failed init to tell wiring/signal problems apart from logic errors. */
static void spi_selftest(void)
{
    static const uint8_t pattern[8] = {0x00, 0xFF, 0x55, 0xAA, 0xAB, 0x56, 0x0F, 0xF0};
    diag.eth_tests = diag.eth_errors = 0;
    diag.mac_tests = diag.mac_errors = 0;
    for (unsigned round = 0; round < 8U && !io_failed; ++round) {
        for (unsigned i = 0; i < sizeof pattern && !io_failed; ++i) {
            uint8_t w = pattern[i], r;
            write_reg(EBSTSD, w);
            r = read_reg(EBSTSD);
            diag.eth_tests++;
            if (r != w) {
                if (diag.eth_errors++ == 0U) {
                    diag.eth_wrote = w;
                    diag.eth_read = r;
                }
            }
            write_reg(0xE4U, w);            /* MAADR1 */
            r = read_reg(0xE4U);
            diag.mac_tests++;
            if (r != w) {
                if (diag.mac_errors++ == 0U) {
                    diag.mac_wrote = w;
                    diag.mac_read = r;
                }
            }
        }
    }
}

static enc_status_t init_fail(enc_init_step_t step, uint8_t index,
                              uint8_t expected, uint8_t got, enc_status_t st)
{
    diag.step = step;
    diag.index = index;
    diag.expected = expected;
    diag.got = got;
    if (step == ENC_STEP_REVISION || step == ENC_STEP_MAC_VERIFY) {
        spi_selftest();
    }
    return st;
}
static enc_status_t chip_init(const uint8_t mac[6])
{
    /* DS39662E register names MAADR1..6 are not in linear register order. */
    static const uint8_t mac_regs[6] = {0xE4, 0xE5, 0xE2, 0xE3, 0xE0, 0xE1};
    unsigned waited;

    if (spi_dirty) {
        /* Recovering from an SPI error: restart the peripheral as well. */
        (void)HAL_SPI_DeInit(enc_spi);
        if (HAL_SPI_Init(enc_spi) != HAL_OK) {
            return init_fail(ENC_STEP_SPI, 0, 0, 0, ENC_ERR_IO);
        }
        spi_dirty = false;
    }
    io_failed = 0;
    bank = 0;
    revision = 0;
    memset(&diag, 0, sizeof diag);

    deselect_enc();
    HAL_GPIO_WritePin(ENC_RESET_GPIO_Port, ENC_RESET_Pin, GPIO_PIN_RESET);
    delay_ms(2);
    HAL_GPIO_WritePin(ENC_RESET_GPIO_Port, ENC_RESET_Pin, GPIO_PIN_SET);
    delay_ms(2);
    select_enc();
    exchange(SRC);
    deselect_enc();
    delay_ms(2);   /* CLKRDY alone is unreliable after reset: DS80349C issue 2. */
    for (waited = 0; !io_failed && (read_reg(ESTAT) & CLKRDY) == 0U; ++waited) {
        if (waited >= 10U) {
            return init_fail(ENC_STEP_CLKRDY, 0, CLKRDY, read_reg(ESTAT), ENC_ERR_CHIP);
        }
        delay_ms(1);
    }
    if (io_failed) {
        return init_fail(ENC_STEP_SPI, 0, 0, 0, ENC_ERR_IO);
    }
    revision = read_reg(EREVID);
    if (io_failed) {
        return init_fail(ENC_STEP_SPI, 0, 0, 0, ENC_ERR_IO);
    }
    /* 0x00/0xFF: no chip / MISO stuck. B1 (2) and B4 (4) need SCK >= 8 MHz
     * for reliable MAC/MII access (DS80349C issue 1); we run 5.25 MHz. */
    if (revision < 5U || revision == 0xFFU) {
        return init_fail(ENC_STEP_REVISION, 0, 0x06, revision, ENC_ERR_CHIP);
    }

    write_reg(EIE, 0);                    /* no INT while configuring */
    operation(BFS, ECON2, AUTOINC);
    write16(ERXSTL, RX_START);
    write16(ERXNDL, RX_END);
    write16(ERXRDPTL, RX_END);
    next_packet = RX_START;
    write16(ETXSTL, TX_START);
    write_reg(ERXFCON, 0xA1U);            /* Unicast or broadcast, CRC required. */
    write_reg(MACON1, 0x01U);             /* MARXEN, no pause frames in half duplex. */
    write_reg(MACON3, 0x32U);             /* Pad, append CRC, length check, FULDPX = 0. */
    write_reg(MACON4, 0x40U);             /* DEFER (half duplex). */
    write_reg(MABBIPG, 0x12U);            /* half-duplex back-to-back gap */
    write_reg(MAIPGL, 0x12U);
    write_reg(MAIPGH, 0x0CU);
    write16(MAMXFLL, ENC28J60_MAX_FRAME + 4U);
    for (unsigned i = 0; i < 6; ++i) {
        write_reg(mac_regs[i], mac[i]);
    }
    write_phy(PHCON1, 0x0000U);           /* PDPXMD = 0: half duplex. */
    write_phy(PHCON2, HDLDIS);            /* disable half-duplex loopback. */
    write_phy(PHIE, PGEIE | PLNKIE);      /* PHY link-change interrupt -> EIR.LINKIF */
    (void)read_phy(PHIR);                 /* clear any latched PHY interrupt */
    for (unsigned i = 0; i < 6; ++i) {
        uint8_t got = read_reg(mac_regs[i]);
        if (io_failed) {
            return init_fail(ENC_STEP_SPI, 0, 0, 0, ENC_ERR_IO);
        }
        if (got != mac[i]) {
            for (unsigned k = 0; k < 6; ++k) {       /* dump all six for the log */
                diag.mac_dump[k] = read_reg(mac_regs[k]);
            }
            return init_fail(ENC_STEP_MAC_VERIFY, (uint8_t)i, mac[i], got, ENC_ERR_CHIP);
        }
    }
    operation(BFC, EIR, ENC_EIR_RXERIF | ENC_EIR_TXERIF | ENC_EIR_TXIF);
    write_reg(EIE, INTIE | PKTIE | LINKIE | RXERIE);
    operation(BFS, ECON1, RXEN);
    if (io_failed) {
        return init_fail(ENC_STEP_SPI, 0, 0, 0, ENC_ERR_IO);
    }
    return ENC_OK;
}

enc_status_t enc28j60_init(SPI_HandleTypeDef *spi, const uint8_t mac[6])
{
    enc_status_t st;
    if (spi == NULL || mac == NULL) {
        return ENC_ERR_ARG;
    }
    if (enc_mutex == NULL) {
        enc_mutex = xSemaphoreCreateMutex();
        if (enc_mutex == NULL) {
            return ENC_ERR_NOT_READY;
        }
    }
    ready = false;
    lock();
    enc_spi = spi;
    if (initialised_once) {
        stats.reinits++;
    }
    initialised_once = true;
    st = chip_init(mac);
    if (st == ENC_ERR_IO) {
        spi_dirty = true;
    }
    ready = (st == ENC_OK);
    unlock();
    return st;
}

uint8_t enc28j60_revision(void)
{
    return revision;
}

const char *enc28j60_revision_name(uint8_t rev)
{
    switch (rev) {
    case 0x02: return "B1";
    case 0x04: return "B4";
    case 0x05: return "B5";
    case 0x06: return "B7";
    default:   return "unknown";
    }
}

enc_status_t enc28j60_read_mac(uint8_t mac[6])
{
    static const uint8_t mac_regs[6] = {0xE4, 0xE5, 0xE2, 0xE3, 0xE0, 0xE1};
    if (enc_mutex == NULL) {
        return ENC_ERR_NOT_READY;
    }
    lock();
    for (unsigned i = 0; i < 6; ++i) {
        mac[i] = read_reg(mac_regs[i]);
    }
    unlock();
    return io_failed ? ENC_ERR_IO : ENC_OK;
}

bool enc28j60_ready(void)
{
    return ready && !io_failed;
}

bool enc28j60_check_alive(void)
{
    uint8_t econ1;
    if (!enc28j60_ready()) {
        return false;
    }
    lock();
    econ1 = read_reg(ECON1);
    if (!io_failed && (econ1 == 0xFFU || (econ1 & RXEN) == 0U)) {
        /* chip was reset behind our back (brown-out, glitch on RESET) */
        io_failed = 1;
        ready = false;
    }
    unlock();
    return enc28j60_ready();
}

bool enc28j60_link_up(void)
{
    uint16_t phstat2;
    if (!enc28j60_ready()) {
        return false;
    }
    lock();
    phstat2 = read_phy(PHSTAT2);
    unlock();
    return !io_failed && (phstat2 & LSTAT) != 0U;
}

/* ------------------------------------------------------------------------
 * interrupts / RX
 * --------------------------------------------------------------------- */
uint8_t enc28j60_irq_begin(void)
{
    uint8_t eir;
    if (!enc28j60_ready()) {
        return 0;
    }
    lock();
    operation(BFC, EIE, INTIE);        /* releases INT; re-armed in irq_end() */
    eir = read_reg(EIR);
    if (eir & ENC_EIR_LINKIF) {
        (void)read_phy(PHIR);          /* reading PHIR clears PGIF and LINKIF */
    }
    unlock();
    return io_failed ? 0 : eir;
}

void enc28j60_irq_end(uint8_t eir)
{
    if (!enc28j60_ready()) {
        return;
    }
    lock();
    if (eir & ENC_EIR_RXERIF) {
        operation(BFC, EIR, ENC_EIR_RXERIF);
        stats.rx_overflows++;
    }
    operation(BFS, EIE, INTIE);        /* INT re-asserts if a flag is still pending */
    unlock();
}

uint8_t enc28j60_packet_count(void)
{
    uint8_t count;
    if (!enc28j60_ready()) {
        return 0;
    }
    lock();
    count = read_reg(EPKTCNT);
    unlock();
    return io_failed ? 0 : count;
}

int enc28j60_receive(enc_rx_alloc_fn alloc, void *ctx)
{
    uint8_t header[6];
    uint16_t following, wire_length, status, length = 0;
    enc_seg_t segs[ENC28J60_MAX_SEGS];
    int result = 0;

    if (!enc28j60_ready()) {
        return -1;
    }
    lock();
    if (read_reg(EPKTCNT) == 0U || io_failed) {
        unlock();
        return io_failed ? -1 : 0;
    }
    /* The whole packet is read under the mutex: ERDPT is shared with the
     * TX status-vector read in enc28j60_send(). */
    write16(ERDPTL, next_packet);
    read_buffer(header, sizeof header);
    following = (uint16_t)(header[0] | ((uint16_t)header[1] << 8));
    wire_length = (uint16_t)(header[2] | ((uint16_t)header[3] << 8));
    status = (uint16_t)(header[4] | ((uint16_t)header[5] << 8));
    if (io_failed) {
        unlock();
        return -1;
    }
    if (following > RX_END || (following & 1U) || following == next_packet) {
        reset_rx();
        stats.rx_resets++;
        unlock();
        return io_failed ? -1 : 0;
    }
    if ((status & 0x80U) && wire_length >= 18U &&
        wire_length <= ENC28J60_MAX_FRAME + 4U) {
        unsigned n;
        length = wire_length - 4U;          /* strip FCS */
        n = alloc(ctx, length, segs, ENC28J60_MAX_SEGS);
        if (n > 0U) {
            for (unsigned i = 0; i < n; ++i) {
                read_buffer(segs[i].data, segs[i].len);   /* ERDPT auto-increments/wraps */
            }
            stats.rx_frames++;
            result = (int)length;
        } else {
            stats.rx_dropped++;
        }
    } else {
        stats.rx_bad++;
    }
    /* Always release the packet, including invalid/oversize/dropped frames. */
    next_packet = following;
    write16(ERXRDPTL, following == RX_START ? RX_END : following - 1U);
    operation(BFS, ECON2, PKTDEC);
    unlock();
    return io_failed ? -1 : result;
}

/* ------------------------------------------------------------------------
 * TX
 * --------------------------------------------------------------------- */
typedef enum { TXW_DONE, TXW_ERROR, TXW_TIMEOUT, TXW_IO } tx_wait_t;

/* Polls for completion with the mutex released between polls, so the RX
 * task can drain packets while the frame is on the wire. */
static tx_wait_t tx_wait(void)
{
    TickType_t start = xTaskGetTickCount();
    unsigned polls = 0;
    for (;;) {
        uint8_t eir, econ1;
        lock();
        eir = read_reg(EIR);
        econ1 = read_reg(ECON1);
        unlock();
        if (io_failed) {
            return TXW_IO;
        }
        if (eir & ENC_EIR_TXERIF) {
            return TXW_ERROR;
        }
        if ((econ1 & TXRTS) == 0U) {
            return TXW_DONE;
        }
        if ((TickType_t)(xTaskGetTickCount() - start) >= pdMS_TO_TICKS(TX_TIMEOUT_MS)) {
            return TXW_TIMEOUT;
        }
        if (++polls > TX_SPIN_POLLS) {
            vTaskDelay(1);
        }
    }
}

static void tx_logic_reset(void)
{
    operation(BFC, ECON1, TXRTS);
    /* DS80349C issue 12: reset TX logic before every (re)transmission. */
    operation(BFS, ECON1, TXRST);
    operation(BFC, ECON1, TXRST);
    operation(BFC, EIR, ENC_EIR_TXERIF | ENC_EIR_TXIF);
}

enc_status_t enc28j60_send(const enc_cseg_t *segs, unsigned nsegs, uint16_t total)
{
    enc_status_t result = ENC_ERR_TX;
    uint16_t sum = 0;

    if (segs == NULL || nsegs == 0U || total < 14U || total > ENC28J60_MAX_FRAME) {
        return ENC_ERR_ARG;
    }
    for (unsigned i = 0; i < nsegs; ++i) {
        sum = (uint16_t)(sum + segs[i].len);
    }
    if (sum != total) {
        return ENC_ERR_ARG;
    }
    if (!enc28j60_ready()) {
        return ENC_ERR_NOT_READY;
    }

    lock();
    tx_logic_reset();
    write16(EWRPTL, TX_START);
    write16(ETXSTL, TX_START);
    write16(ETXNDL, TX_START + total);
    select_enc();
    exchange(WBM);
    exchange(0);  /* Per-packet control byte: use MACON3 padding/CRC settings. */
    for (unsigned i = 0; i < nsegs && !io_failed; ++i) {
        if (segs[i].len != 0U &&
            HAL_SPI_Transmit(enc_spi, (uint8_t *)segs[i].data, segs[i].len,
                             SPI_TIMEOUT_MS) != HAL_OK) {
            fail_io();
        }
    }
    deselect_enc();

    for (unsigned attempt = 0; attempt < TX_ATTEMPTS && !io_failed; ++attempt) {
        tx_wait_t w;
        if (attempt > 0U) {
            stats.tx_retries++;
            tx_logic_reset();          /* frame is still in the TX buffer */
        }
        operation(BFS, ECON1, TXRTS);
        unlock();
        w = tx_wait();
        lock();
        if (w == TXW_DONE) {
            result = ENC_OK;
            break;
        }
        if (w == TXW_IO) {
            break;
        }
        /* TXERIF (excessive/late collision, abort) or TXRTS stuck: the
         * errata says late collisions in half duplex are not retried by
         * hardware and TXRTS may stay set, so reset TX logic and resend.
         * Status vector (TSV) at ETXND+1, byte 3 bit 5 = late collision,
         * bit 4 = excessive collisions; read only for diagnostics. */
        {
            uint8_t tsv[7];
            write16(ERDPTL, (uint16_t)(TX_START + total + 1U));
            read_buffer(tsv, sizeof tsv);
            (void)tsv;
        }
    }
    if (io_failed) {
        result = ENC_ERR_IO;
    }
    if (result == ENC_OK) {
        stats.tx_frames++;
    } else {
        tx_logic_reset();
        stats.tx_failed++;
    }
    unlock();
    return result;
}

/* ------------------------------------------------------------------------
 * Diagnostic sweep (only called after a failed init, never in normal use).
 *
 * Temporarily overrides the CubeMX SPI3 baud-rate prescaler and the output
 * speed of the SCK/MOSI pins at run time, repeats a MAC-register
 * write/read-back test for every combination, then restores both. The
 * result tells which CubeMX setting gives an error-free link on this
 * wiring. SCK = PC10, MOSI = PC12 (SPI3) are hard-coded here on purpose:
 * this is a bring-up aid, not part of the driver proper.
 * --------------------------------------------------------------------- */
#define DIAG_SPI_GPIO      GPIOC
#define DIAG_SCK_PIN_NO    10U
#define DIAG_MOSI_PIN_NO   12U

static void diag_set_prescaler(uint32_t br_bits)
{
    __HAL_SPI_DISABLE(enc_spi);
    MODIFY_REG(enc_spi->Instance->CR1, SPI_CR1_BR, br_bits);
    __HAL_SPI_ENABLE(enc_spi);
}

static void diag_set_pin_speed(uint32_t speed)
{
    uint32_t v = DIAG_SPI_GPIO->OSPEEDR;
    v &= ~((3UL << (2U * DIAG_SCK_PIN_NO)) | (3UL << (2U * DIAG_MOSI_PIN_NO)));
    v |= (speed << (2U * DIAG_SCK_PIN_NO)) | (speed << (2U * DIAG_MOSI_PIN_NO));
    DIAG_SPI_GPIO->OSPEEDR = v;
}

/* Writes the six MAC bytes, reads them back; repeated. Returns bad reads.
 * with_phy: do the same PHY accesses as chip_init() between the writes and
 * the read-back, to reproduce the exact sequence that failed. */
static uint16_t diag_mac_test(const uint8_t mac[6], unsigned repeats, bool with_phy,
                              uint16_t per_byte[6], uint8_t *first_idx,
                              uint8_t *first_got)
{
    static const uint8_t mac_regs[6] = {0xE4, 0xE5, 0xE2, 0xE3, 0xE0, 0xE1};
    uint16_t errors = 0;
    for (unsigned r = 0; r < repeats && !io_failed; ++r) {
        for (unsigned i = 0; i < 6; ++i) {
            write_reg(mac_regs[i], mac[i]);
        }
        if (with_phy) {
            write_phy(PHCON1, 0x0000U);
            write_phy(PHCON2, HDLDIS);
            write_phy(PHIE, PGEIE | PLNKIE);
            (void)read_phy(PHIR);
        }
        for (unsigned i = 0; i < 6; ++i) {
            uint8_t got = read_reg(mac_regs[i]);
            if (got != mac[i]) {
                if (errors == 0U && first_idx != NULL) {
                    *first_idx = (uint8_t)i;
                    *first_got = got;
                }
                errors++;
                if (per_byte != NULL) {
                    per_byte[i]++;
                }
            }
        }
    }
    return errors;
}

void enc28j60_spi_sweep(const uint8_t mac[6], enc_sweep_t *out)
{
    /* SPI_CR1.BR for /4, /8, /16, /32 */
    static const uint32_t br[ENC_SWEEP_RATES] = {
        SPI_BAUDRATEPRESCALER_4, SPI_BAUDRATEPRESCALER_8,
        SPI_BAUDRATEPRESCALER_16, SPI_BAUDRATEPRESCALER_32
    };
    uint32_t saved_cr1, saved_ospeedr;

    memset(out, 0, sizeof *out);
    if (enc_mutex == NULL || enc_spi == NULL) {
        return;
    }
    lock();
    saved_cr1 = enc_spi->Instance->CR1 & SPI_CR1_BR;
    saved_ospeedr = DIAG_SPI_GPIO->OSPEEDR;
    out->reads = (uint16_t)(6U * ENC_SWEEP_REPEATS);

    /* 1. As configured by CubeMX, with per-byte error counts. */
    out->as_configured = diag_mac_test(mac, ENC_SWEEP_REPEATS, false, out->per_byte,
                                       &out->first_idx, &out->first_got);
    out->as_configured_phy = diag_mac_test(mac, ENC_SWEEP_REPEATS, true,
                                           out->per_byte_phy, &out->first_idx_phy,
                                           &out->first_got_phy);
    /* 2. Every prescaler x pin-speed combination. */
    for (unsigned r = 0; r < ENC_SWEEP_RATES; ++r) {
        for (unsigned sp = 0; sp < ENC_SWEEP_SPEEDS; ++sp) {
            diag_set_prescaler(br[r]);
            diag_set_pin_speed(sp);
            out->errors[r][sp] = diag_mac_test(mac, ENC_SWEEP_REPEATS, true,
                                               NULL, NULL, NULL);
        }
    }
    DIAG_SPI_GPIO->OSPEEDR = saved_ospeedr;
    diag_set_prescaler(saved_cr1);
    out->spi_failed = io_failed != 0U;
    unlock();
}

void enc28j60_get_diag(enc_diag_t *out)
{
    taskENTER_CRITICAL();
    *out = diag;
    taskEXIT_CRITICAL();
}

const char *enc28j60_step_name(uint8_t step)
{
    switch (step) {
    case ENC_STEP_NONE:       return "none";
    case ENC_STEP_SPI:        return "SPI transfer (HAL error/timeout)";
    case ENC_STEP_CLKRDY:     return "ESTAT.CLKRDY not set";
    case ENC_STEP_REVISION:   return "EREVID check";
    case ENC_STEP_MAC_VERIFY: return "MAC address read-back";
    default:                  return "?";
    }
}

void enc28j60_get_stats(enc_stats_t *out)
{
    taskENTER_CRITICAL();
    *out = stats;
    taskEXIT_CRITICAL();
}
