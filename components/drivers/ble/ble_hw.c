/* Hardware BLE receive: the ESP32-S3 link-layer core (RivieraWaves BLE v9 at 0x60031000) driven
 * without the vendor stack. How it was found: docs/PHY_STANDALONE.md, bench/idf_bleprobe,
 * bench/idf_blegen. Setup data (core registers, BT modem block, baseband sequencer, coex page,
 * exchange-memory image with the scan control structure and RX ring) is generated from a vendor
 * capture by tools/gen_ble_state.py into ble_state.h.
 *
 * Exchange memory (EM): the core addresses its own 16-bit offset space, mapped onto DRAM by page
 * registers 0x60031204+ ([31:18] EM offset / 4, [17:0] DRAM word address). Used here:
 *   0x0000 exchange table entry 0 (mode/status, start time in half-slots, CS pointer / 2)
 *   0x0400 control structure (scan): +0x16 HOPCNTL (channel [5:0], bit 15 auto-hop)
 *   0x1000 RX ring: 10 descriptors of 0x14 bytes: +0 next | done(15), +4 header, +6 RSSI | channel << 8,
 *          +8 CLKN at sync, +0x12 payload pointer
 * Core: 0x0C interrupt mask, 0x14 raw status (0 start, 2 RX, 5 event end, 8 skipped), 0x18 ack,
 *       0x1C SLOTCLK (bit 31 samples), 0x24 RX descriptor pointer, 0x100 ACTSCHCNTL (bit 31 starts entry). */
#include "ble_hw.h"
#include "irq.h"
#include "swtimer.h"
#include "console.h"
#include "soc_intr.h"
#include "xtensa.h"
#include "sys.h"
#include <string.h>
#include "../rf/rf_pll.h"
#include "ble_state.h"
#include "aes.h"
#include "rng.h"

#define R32(a)  (*(volatile uint32_t *)(a))
#define CORE(o) R32(0x60031000u + (o))
#define SRC_RWBLE 8
#define RING0   0x1000u
#define RING_N  10u                              /* descriptors the vendor image has (advertising / scanning windows use these) */
#define RING_CN 48u                              /* a connection event: one descriptor per PDU exchanged, so this bounds the PDUs per event (10 held it to ~13) */
#define RXPAY   0x3400u                          /* EM pages for the payloads of descriptors 10..RING_CN-1: 10..17 hold a PDU of the data length extension (288 bytes each), 18.. a classic one (40) */
static uint16_t rx_pay(unsigned k) { return (uint16_t)(RXPAY + 40u * (k - RING_N) + 5); }
#define INT_END (1u << 5)
#define INT_SKIP (1u << 8)
#define INT_RX   (1u << 2)                       /* a PDU received: inside a connection event, the chain is topped up (tx_load) */
#ifndef BLE_FINE
#define BLE_FINE 500                              /* kick-to-start margin, half-us */
#endif
#ifndef BLE_MAXEV
#define BLE_MAXEV 0x40
#endif
#ifndef BLE_MASK
#define BLE_MASK (INT_END | INT_SKIP)
#endif

static uint32_t em_buf[BLE_EM_WORDS];
static uint32_t ring_buf[256];                /* EM 0x1000..0x13FF: the RX ring, RING_CN descriptors of 0x14 bytes (the vendor image has 10) */
static uint32_t rxpay_buf[512];               /* EM RXPAY..: payloads of descriptors 10..RING_CN-1, 40 bytes each (a classic PDU of 27 bytes + header) */
static volatile unsigned ring_n = RING_N;     /* descriptors in use: RING_N while advertising / scanning, RING_CN in a connection */
static volatile uint8_t scan_chan;            /* 0: stopped */
static volatile uint32_t last_end;            /* ccount of the last event end (watchdog) */
static volatile uint8_t adv_on;              /* advertising instead of scanning */
static uint32_t adv_next, adv_iv;             /* next advert start / interval, half-slots */
static uint16_t scan_cs[45];                  /* scan control structure, restored by ble_hw_scan */
static uint32_t tx_buf[1024];                  /* EM 0x2400 (page register 7, one 1 KB page): +0 advertising payload, +0x40.. data TX
                                                * (raw: up to 255 bytes), +0x200 / +0x300 raw RX buffers */
#define CS0     0x400u
#define TXD     0x1400u
#define TXBUF   0x2400u
static ble_hw_stats_t st;

/* ---- connections: up to BLE_NCONN links, each in either role -------------------------------------------------------------
 * Everything a link owns lives in conn_t. C is the connection the code at hand works on: the event-end interrupt sets it to the connection whose event ended (and restores it on exit), the
 * public functions set it to the one ble_hw_select picked. Each link has its own control structure and TX chain in exchange memory; link 0 has the big ones (48 descriptors, the long-PDU
 * pool), the others a small chain in the gap of the TX payload pages (classic 27-byte PDUs only). One core, one event at a time: sched() arms the connection whose window is next, and
 * lets a scan window (initiating) or an advert run in the gap before it. */
#define TXC     0x1440u                          /* link 0: first data TX descriptor; the chain is TXD_N descriptors of 16 bytes (EM page 4 holds 0x1400..0x17FF) */
#define TXCBUF  (TXBUF + 0x40u)
#define TXD_N0  48u                              /* link 0: descriptors; a PDU of up to 27 bytes uses the 28-byte slot of its descriptor (TXCBUF + 28 i), a longer one a 256-byte slot of the pool at TXBUF + 0x800 */
#define TXB_N0  8u
#define TXB(b)  (TXBUF + 0x800u + 256u * (b))
#define TXC1    0x2980u                          /* links 1..: 12 descriptors + their 28-byte slots + the idle descriptor, in the gap between link 0's slots and the long-PDU pool */
#define TXD_N1  12u
#define TXDSC(i)  (C->txc + 16u * (i))             /* descriptor +0 next (bit 15 = done), +2 header (length << 8 | MD 0x10 | LLID), +4 payload pointer */
#define TXDBUF(i) (C->txcbuf + 28u * (i))
#define TXQ_BYTES 8192u                          /* software TX queue: entries [llid, length, payload...] in a byte ring, so 27-byte and 251-byte PDUs share it */
typedef struct {
    uint16_t cs, txc, txcbuf, idle, nd, nb;      /* exchange-memory layout of this link: control structure, TX chain, its slots, the master's idle descriptor, descriptor and long-slot counts */
    volatile uint8_t on, term_tx, term_req;      /* term_tx: ble_hw_disconnect queued LL_TERMINATE_IND, the link ends once it is acknowledged; term_req: LL_TERMINATE_IND seen, ended at the event end (the core is idle there) */
    uint8_t tx_first, tx_cnt, big_first, big_cnt, desc_big[TXD_N0];     /* descriptors in flight (ring from tx_first); big payload slots in use (ring from big_first) and which descriptor holds one */
    uint16_t tx_max_oct;                         /* negotiated octets we may send (the central's receive size); we receive classic 27-byte PDUs only */
    struct {
        uint32_t aa, crcinit;
        uint16_t interval, latency, timeout, evt, instant_upd, instant_map;
        uint8_t  csa2, hop, last_unmapped, chm[5], nused, used[37], upd_pending, map_pending, new_chm[5];
        uint16_t new_interval, new_latency, new_timeout, new_winoff; uint8_t new_winsize;
        uint16_t instant_phy; uint8_t phy_pending, new_tx_phy, new_rx_phy, phy_tx, phy_rx;   /* PHY indices: 0 = 1M, 1 = 2M */
        uint64_t anchor;                         /* half-us: start of the window of the next event */
        uint32_t win;                            /* half-us: window length */
        uint32_t last_rx_ms;                     /* ms clock of the last PDU from the peer */
        uint8_t master;                          /* we are the central: we start every event (format 2) and own the anchor */
    } c;
    volatile uint32_t txq_head, txq_tail;
    uint8_t txq_buf[TXQ_BYTES];
    uint8_t init_addr[6];
    uint8_t peer_addr7[7], own_addr7[7];         /* type + 6 octets (LSO first) of the connected peer and of our address */
    /* link-layer encryption (ROM r_lld_con_enc_key_load / r_lld_con_tx_enc / r_lld_con_rx_enc): key at CS+0x2C, IV at CS+0x3C, packet counters at CS+0x44; CS+2 bit 8 decrypts, bit 9 encrypts */
    volatile uint8_t enc_notify;                 /* LL_START_ENC_RSP answered: tell the host once transmit encryption is on */
    volatile uint8_t enc_block;                  /* LL_START_ENC_REQ queued: no data PDU is accepted until transmit encryption is on, the first encrypted PDU must be LL_START_ENC_RSP */
    volatile uint8_t enc_tx_pend;                /* LL_START_ENC_RSP queued: transmit encryption starts at the next event end, together with the first PDU (an empty PDU must not be encrypted) */
    uint8_t enc_state, tx_enc, rx_enc, mic_fail; /* enc_state: 0 off, 1 key loaded and receive decryption on, waiting for LL_START_ENC_RSP, 2 both ways on */
    uint64_t last_sync;
    uint32_t missed;
} conn_t;
_Static_assert(BLE_NCONN <= 2, "the TX gap holds one more chain");
static conn_t conns[BLE_NCONN], *C = conns;
static conn_t *ev_c;                             /* the connection whose event is armed (NULL: an advert, scan or initiating window) */
static uint32_t et_idx;                          /* exchange-table entry rotation */
static uint8_t sel, last_conn;                              /* ble_hw_select: the link the public functions work on */
#define cn          (C->c)
#define CS1         (C->cs)
#define IDLE        (C->idle)
#define TXD_N       (C->nd)
#define TXB_N       (C->nb)
#define conn_on     (C->on)
#define term_tx     (C->term_tx)
#define term_req    (C->term_req)
#define tx_first    (C->tx_first)
#define tx_cnt      (C->tx_cnt)
#define big_first   (C->big_first)
#define big_cnt     (C->big_cnt)
#define desc_big    (C->desc_big)
#define tx_max_oct  (C->tx_max_oct)
#define txq_head    (C->txq_head)
#define txq_tail    (C->txq_tail)
#define txq_buf     (C->txq_buf)
#define init_addr   (C->init_addr)
#define peer_addr7  (C->peer_addr7)
#define own_addr7   (C->own_addr7)
#define enc_notify  (C->enc_notify)
#define enc_block   (C->enc_block)
#define enc_tx_pend (C->enc_tx_pend)
#define enc_state   (C->enc_state)
#define tx_enc      (C->tx_enc)
#define rx_enc      (C->rx_enc)
#define mic_fail    (C->mic_fail)
#define last_sync   (C->last_sync)
#define missed      (C->missed)
static uint8_t own_addr[6];
#define txq_at(k) (txq_buf[(txq_tail + (k)) % TXQ_BYTES])
static int send_c(uint8_t llid, const uint8_t *data, unsigned len);
static void conns_off(void);
static int nconn_on(void) { int n = 0; for (int k = 0; k < BLE_NCONN; k++) n += conns[k].on; return n; }
static void conns_off(void) { for (int k = 0; k < BLE_NCONN; k++) conns[k].on = 0; ev_c = 0; C = conns; ring_n = RING_N; }
static conn_t *conn_free(void) { for (int k = 0; k < BLE_NCONN; k++) if (!conns[k].on) return &conns[k]; return 0; }
static void conns_init(void)
{
    for (int k = 0; k < BLE_NCONN; k++) {
        conn_t *c = &conns[k];
        c->cs = (uint16_t)(0x45Au + 0x5Au * k);
        if (!k) { c->txc = TXC; c->txcbuf = TXCBUF; c->idle = 0x1780u; c->nd = TXD_N0; c->nb = TXB_N0; }
        else { c->txc = (uint16_t)(TXC1 + 0x220u * (k - 1)); c->nd = TXD_N1; c->nb = 0; c->txcbuf = (uint16_t)(c->txc + 16u * TXD_N1); c->idle = (uint16_t)(c->txcbuf + 28u * TXD_N1); }
    }
    C = conns; for (int k = 0; k < BLE_NCONN; k++) { C = &conns[k]; tx_max_oct = 27; }
    C = conns;
}
static int (*enc_ltk_cb)(const uint8_t rand[8], uint16_t ediv, uint8_t ltk[16]);
static void (*enc_done_cb)(int on);
void ble_hw_set_enc(int (*ltk)(const uint8_t rand[8], uint16_t ediv, uint8_t ltk[16]), void (*done)(int on)) { enc_ltk_cb = ltk; enc_done_cb = done; }
static uint32_t ms_now(void) { return (uint32_t)(time_us() / 1000); }
#define Q_N 32u
static ble_hw_pkt_t q[Q_N];
static volatile uint32_t q_head, q_tail;

static uint32_t em_addr(uint32_t off)
{
    if (off >= 0x1000 && off < 0x1400) return (uint32_t)ring_buf + off - 0x1000;
    if (off >= RXPAY && off < RXPAY + 0x800) return (uint32_t)rxpay_buf + off - RXPAY;
    if (off >= TXBUF && off < TXBUF + sizeof tx_buf) return (uint32_t)tx_buf + off - TXBUF;     /* TX payload pages (the chains of the links 1.. live in them too) */
    unsigned b = 0;
    for (unsigned k = 0; k < sizeof ble_em_segs / sizeof ble_em_segs[0]; k++)
        if (ble_em_segs[k].em_off <= off && ble_em_segs[k].em_off >= ble_em_segs[b].em_off) b = k;
    return (uint32_t)em_buf + ble_em_segs[b].pos + off - ble_em_segs[b].em_off;
}
#define EM16(off) (*(volatile uint16_t *)em_addr(off))
/* offsets come from descriptors the radio writes: a span must lie inside em_buf before anything is copied */
static bool em_span_ok(uint32_t off, uint32_t n)
{
    uint32_t a = em_addr(off);
    const struct { uint32_t b, sz; } r[] = { { (uint32_t)em_buf, sizeof em_buf }, { (uint32_t)ring_buf, sizeof ring_buf }, { (uint32_t)rxpay_buf, sizeof rxpay_buf }, { (uint32_t)tx_buf, sizeof tx_buf } };
    for (unsigned k = 0; k < 4; k++) if (a >= r[k].b && a + n <= r[k].b + r[k].sz && a + n >= a) return true;
    return false;
}

static uint32_t clkn(void)
{
    CORE(0x1C) = (CORE(0x1C) & 0x7FFFFFFFu) | 0x80000000u;
    while (CORE(0x1C) & 0x80000000u) {}
    return CORE(0x1C) & 0x0FFFFFFFu;
}

static void drain(void)
{
    conn_t *sv = C; if (ev_c) C = ev_c;                           /* a connection event: its flags and keys apply */
    const int in_conn = ev_c != 0;
    for (uint32_t d = RING0; d < RING0 + ring_n * 0x14; d += 0x14) {
        if (!(EM16(d) & 0x8000)) continue;
        /* RX status [3:0]: sync, RX time, length, CRC error. In a connection also bit 5 (NESN error) and bit 6 (SN error: the central resent a PDU
         * we already have), as ROM r_lld_con_rx drops them (mask 0x66 + bits 0, 3; bench/rfmeas/data/rom_lld_con_rx.dis): delivered, a resent
         * ATT request was answered twice and the 2nd answer met the client's next request (NimBLE EBADDATA, discovery failed 1 in ~30 connections).
         * While advertising, bit 6 is a normal flag. */
        if (in_conn && (EM16(d + 2) & 0x10)) { mic_fail = 1; EM16(d) &= 0x7FFF; EM16(d + 4) = 0; continue; }      /* RX status bit 4: MIC failure (ROM r_lld_con_rx ends the link) */
        if (EM16(d + 2) & (in_conn ? 0x006F : 0x000F)) {
            st.rx_errors++; EM16(d) &= 0x7FFF; EM16(d + 4) = 0; continue;
        }
        uint32_t n = (q_head + 1) % Q_N;
        if (in_conn && cn.evt == 0 && em_span_ok(EM16(d + 0x12), 6) && !memcmp((const void *)em_addr(EM16(d + 0x12)), init_addr, 6)) { EM16(d) &= 0x7FFF; EM16(d + 4) = 0; continue; }
        if (n == q_tail) st.dropped++;
        else {
            ble_hw_pkt_t *p = &q[q_head];
            uint16_t ph = EM16(d + 4), ch = EM16(d + 6);
            p->conn = in_conn ? (uint8_t)(C - conns) : 0xFF;
            p->hdr0 = ph & 0xFF; p->len = ph >> 8; p->rssi = (int8_t)(ch & 0xFF); p->chan = (ch >> 8) & 0x3F;
            p->clkn = EM16(d + 8) | (uint32_t)EM16(d + 10) << 16;
            uint32_t len = p->len;
            if (in_conn && rx_enc && len >= 5) { len -= 4; p->len = (uint8_t)len; }              /* a decrypted PDU's length includes the MIC */
            p->len = len;                                /* consumers see what was copied, never more */
            uint16_t src = EM16(d + 0x12);
            if (em_span_ok(src, len)) memcpy(p->data, (const void *)em_addr(src), len); else { EM16(d) &= 0x7FFF; continue; }
            q_head = n;
        }
        st.packets++;
        EM16(d) &= 0x7FFF; EM16(d + 4) = 0;
    }
    C = sv;
}

/* one scan event (one receive window) starting at the next half-slot. Drains first: the core can set a
 * descriptor's done bit after the end-of-event interrupt, and an event kicked onto a done descriptor
 * never receives and never ends (measured). */
static void arm(void)
{
    drain(); ev_c = 0;
    EM16(0x416) = scan_chan;                     /* fixed channel, no hop */
    EM16(0x42E) = BLE_MAXEV;                     /* MAXEVTIME */
    EM16(0x420) = RING_N;                        /* RX buffers per window (vendor: 1): one window takes up to RING_N PDUs */
    /* start 250 us after the kick: CLKN half-slot + fine target (0x60031020 counts down 623..0, 0.5 us).
     * Measured: 150 us is too soon (every event skipped), 200..250 us best (1190 PDUs/s vs 935 with
     * half-slot starts), 300 us slower. */
    uint32_t t = clkn(), cnt = CORE(0x20) & 0x3FF, f;
    if (cnt > BLE_FINE) f = cnt - BLE_FINE; else { f = cnt + 0x270u - BLE_FINE; t++; }
    EM16(0) = EM16(0) & ~0x38u;
    EM16(2) = t & 0xFFFF; EM16(4) = (t >> 16) & 0xFFF; EM16(6) = f;
    CORE(0x24) = RING0;
    CORE(0x18) = 0xFFFFFFFFu;                    /* stale status blocks the next start (measured) */
    CORE(0x100) = 0x80000000u;
}

/* Interrupt: mask the core's interrupt, handle, unmask. Skipped events (bit 8) must be handled as well as
 * ended ones: an unhandled skip leaves the scheduler idle with no further interrupt (measured). */
static void service_(void);
static void service(void) { conn_t *sv = C; service_(); C = sv; }       /* the interrupt must not leave another link selected under the code it interrupted */
static void isr(void *arg)
{
    (void)arg; st.isr_calls++; CORE(0x0C) = 0;
    service();
}

static uint8_t adv_multi;                     /* keep advertising while a link slot is free, so a second central can connect */
void ble_hw_adv_multi(int on)
{
    uint32_t ps = irq_lock();
    adv_multi = (uint8_t)(on != 0);
    if (!adv_multi) adv_on = 0;
    irq_unlock(ps);
}
static uint8_t adv_csa2 = 1;                  /* advertise CSA#2 support (ChSel); ble_hw_csa2(0) forces CSA#1 */
void ble_hw_csa2(int on) { adv_csa2 = (uint8_t)on; }
static uint8_t adv_type = 2;                  /* 0 ADV_IND (connectable), 2 ADV_NONCONN_IND */
void ble_hw_adv_type(uint8_t t) { adv_type = t; }
static uint32_t adv_idx;                      /* the vendor rotates exchange-table entries 0..15, one per advert */
static void arm_adv(void)
{
    ev_c = 0;
    uint32_t now = clkn();
    if ((int32_t)(adv_next - now) < 4) adv_next = now + 4;   /* fell behind: next possible slot */
    adv_idx = (adv_idx + 1) & 15;
    uint32_t e = 16 * adv_idx;
    EM16(e + 0) = 0x2802;                        /* mode 2, status standby (vendor 0x281a after processing) */
    EM16(e + 2) = adv_next & 0xFFFF; EM16(e + 4) = (adv_next >> 16) & 0xFFF;
    EM16(e + 6) = 0x270;                         /* fine start = start of the half-slot (vendor) */
    EM16(e + 8) = CS0 / 2;
    EM16(e + 10) = 0x030B; EM16(e + 12) = 0x0C00; EM16(e + 14) = 0x0F00;
    EM16(TXD) = adv_type == 0 ? TXD + 14 : 0;    /* ADV_IND chains to the SCAN_RSP descriptor at +14 (vendor); owned by the core */
    EM16(TXD + 14) = TXD;
    CORE(0x24) = RING0;
    CORE(0x18) = 0xFFFFFFFFu;
    CORE(0x100) = 0x80000000u | adv_idx;
}

static uint64_t now_hu(void)                    /* current core time, half-us */
{
    uint32_t t = clkn(), f = CORE(0x20) & 0x3FF;
    return (uint64_t)t * 625u + (624u - f);
}

static void chm_apply(const uint8_t m[5])
{
    memcpy(cn.chm, m, 5); cn.nused = 0;
    for (int c = 0; c < 37; c++) if (m[c >> 3] & (1 << (c & 7))) cn.used[cn.nused++] = (uint8_t)c;
    EM16(CS1 + 0x22) = m[0] | m[1] << 8; EM16(CS1 + 0x24) = m[2] | m[3] << 8; EM16(CS1 + 0x26) = (EM16(CS1 + 0x26) & 0xFF00) | m[4];
}

static uint8_t csa1_next(void)                   /* channel selection #1 */
{
    cn.last_unmapped = (uint8_t)((cn.last_unmapped + cn.hop) % 37);
    uint8_t c = cn.last_unmapped;
    if (!(cn.chm[c >> 3] & (1 << (c & 7)))) c = cn.used[c % cn.nused];
    return c;
}

static uint8_t csa2_ch(uint16_t cnt)             /* channel selection #2 (Core 5.x Vol 6 B 4.5.8.3) */
{
    uint16_t cid = (uint16_t)((cn.aa >> 16) ^ (cn.aa & 0xFFFF)), p = cnt ^ cid;
    for (int r = 0; r < 3; r++) {
        uint16_t q = 0;
        for (int b = 0; b < 16; b++) if (p & (1u << b)) q |= (uint16_t)(1u << ((b & 8) | (7 - (b & 7))));   /* bit reverse per byte */
        p = (uint16_t)(17u * q + cid);
    }
    uint16_t e = p ^ cid; uint8_t u = e % 37;
    if (cn.chm[u >> 3] & (1 << (u & 7))) return u;
    return cn.used[(cn.nused * (uint32_t)e) >> 16];
}


/* ---- central role: initiating and the master connection ----
 * Reference: the IDF central's structures captured over JTAG (bench/idf_bleprobe/ocd_em_initiator.py, ocd_em_master.py). Initiator CS = format 0x0E (own address at +6, peer
 * address at +0x2C, its type at +0x32, CONNECT_IND descriptor at +0x1C); the core prepends InitA and AdvA to the 22 bytes of LL data, answers a matching connectable ADV_IND with
 * the CONNECT_IND 150 us after it and the event ends. The master connection uses the slave's CS with format 2: every event starts with our packet. */
static const uint16_t ble_init_cs[45] = { 0x060E, 0x0800, 0x2000, 0, 0, 0, 0xBED6, 0x8E89, 0x5555, 0x0055, 0x0040, 0x8025, 0x000B, 0x8010, 0x1400, 0x0026, 0x0010, 0x0028, 0, 0, 0, 0,
                                          0, 0, 0, 0x0001, 0x1400, 0x0026, 0x0028, 0x0026, 0x0028, 0, 0, 0, 0x001D };
static volatile uint8_t init_on, init_chan_i, init_stuck;
static struct { uint8_t peer[6], peer_rand, own[6], own_rand, ll[22]; uint32_t t_ms; } ic;
static void init_arm(void)
{
    drain(); ev_c = 0;
    EM16(0x416) = (uint16_t)(37 + init_chan_i++ % 3);
    /* (+0x2E is not MAXEVTIME in the initiator CS: it is the middle of the peer address) */
    EM16(0x420) = RING_N;
    uint32_t t = clkn(), cnt = CORE(0x20) & 0x3FF, f;
    if (cnt > BLE_FINE) f = cnt - BLE_FINE; else { f = cnt + 0x270u - BLE_FINE; t++; }
    EM16(0) = 0x5002;
    EM16(2) = t & 0xFFFF; EM16(4) = (t >> 16) & 0xFFF; EM16(6) = f;
    EM16(8) = CS0 / 2; EM16(10) = 0x2710; EM16(12) = 0x0C00; EM16(14) = 0x0F00;                 /* a connection leaves entry 0 pointing at its own CS */
    EM16(TXD) = 0; EM16(TXD + 2) = (uint16_t)(34u << 8 | 0x05 | 0x20 | (ic.own_rand ? 0x40 : 0) | (ic.peer_rand ? 0x80 : 0)); EM16(TXD + 4) = TXBUF;     /* CONNECT_IND, ChSel: we do CSA#2 */
    CORE(0x24) = RING0;
    CORE(0x18) = 0xFFFFFFFFu;
    CORE(0x100) = 0x80000000u;
}
static int aa_ok(uint32_t aa)                    /* Core spec vol 6 part B 2.1.2: usable access address */
{
    uint32_t df = aa ^ 0x8E89BED6u;
    if (!df || !(df & (df - 1))) return 0;                       /* the advertising address or one bit away from it */
    unsigned run = 1, tr = 0, b8 = 0;
    for (int i = 1; i < 32; i++) {
        unsigned x = aa >> i & 1, y = aa >> (i - 1) & 1;
        if (x == y) { if (++run > 6) return 0; } else { run = 1; tr++; if (i >= 26 && i < 32) b8++; }
    }
    if (tr > 24) return 0;
    unsigned t6 = 0; for (int i = 26; i < 31; i++) t6 += (aa >> i & 1) != (aa >> (i + 1) & 1);
    if (t6 < 2) return 0;
    unsigned lo16 = 0; for (int i = 1; i < 16; i++) lo16 += (aa >> i & 1) != (aa >> (i - 1) & 1);
    return lo16 >= 3;
}
static void init_setup(void)                     /* the initiator's control structure and the CONNECT_IND's LL data (also after a core reset) */
{
    memcpy(tx_buf, ic.ll, 22);
    for (int k = 0; k < 45; k++) EM16(CS0 + 2 * k) = ble_init_cs[k];
    for (int k = 0; k < 3; k++) EM16(CS0 + 6 + 2 * k) = ic.own[2 * k] | ic.own[2 * k + 1] << 8;
    for (int k = 0; k < 3; k++) EM16(CS0 + 0x2C + 2 * k) = ic.peer[2 * k] | ic.peer[2 * k + 1] << 8;
    EM16(CS0 + 0x32) = (uint16_t)(ic.peer_rand ? 1 : 0);
    EM16(0x14C) = EM16(CS0);
}
int ble_hw_connect(const uint8_t peer[6], int peer_random, const uint8_t own[6], int own_random, uint16_t interval, uint16_t latency, uint16_t timeout)
{
    uint32_t ps = irq_lock();
    if (init_on || !conn_free()) { irq_unlock(ps); return -1; }
    memcpy(ic.peer, peer, 6); ic.peer_rand = (uint8_t)(peer_random != 0); memcpy(ic.own, own, 6); ic.own_rand = (uint8_t)(own_random != 0);
    uint32_t aa; do rng_fill(&aa, 4); while (!aa_ok(aa));
    uint8_t crc[3]; rng_fill(crc, 3);
    uint8_t *l = ic.ll;
    l[0] = (uint8_t)aa; l[1] = (uint8_t)(aa >> 8); l[2] = (uint8_t)(aa >> 16); l[3] = (uint8_t)(aa >> 24);
    memcpy(l + 4, crc, 3); l[7] = 8; l[8] = 12; l[9] = 0;                                     /* WinSize 8 (10 ms). The core overwrites WinOffset on the air (0x24..0x26 slots seen, as with the IDF central: it is computed by hardware and not readable), so the first packet goes out inside every such window */
    l[10] = (uint8_t)interval; l[11] = (uint8_t)(interval >> 8); l[12] = (uint8_t)latency; l[13] = (uint8_t)(latency >> 8); l[14] = (uint8_t)timeout; l[15] = (uint8_t)(timeout >> 8);
    memset(l + 16, 0xFF, 4); l[20] = 0x1F; l[21] = 11;                                        /* all 37 channels, hop 11, SCA 0 (500 ppm) */
    scan_chan = 0; adv_on = 0;
    init_setup();
    ic.t_ms = ms_now(); init_on = 1; last_end = ccount();
    if (!nconn_on()) init_arm();                  /* with links up the next event end schedules the scan window into the gap */
    irq_unlock(ps);
    return 0;
}
void ble_hw_connect_cancel(void) { init_on = 0; }
int ble_hw_is_master(void) { C = &conns[sel]; return conn_on && cn.master; }
void ble_hw_disconnect(void)
{
    uint8_t t[2] = { 0x02, 0x13 };                               /* LL_TERMINATE_IND, remote user terminated */
    C = &conns[sel];
    if (!conn_on) return;
    uint32_t ps = irq_lock(); enc_block = 0; irq_unlock(ps);
    if (!send_c(3, t, 2)) term_tx = 1;
}



/* CS+0x20 is the longest the event may run, in slots (IDF: 0x49 = 73 at a 50 ms interval); the core ends the event once the exchanges add up to it, whatever the RX ring holds. It was 48 here
 * (the ring size), which cut 251-byte PDUs after 13 exchanges (30 ms): 148 notifications/s against IDF's 359. Leave the IDF margin before the next anchor; share the air with the other link,
 * adverts and initiating windows: half the interval. */
static uint16_t maxev_slots(void)
{
    uint32_t iv = 2u * cn.interval, m = iv > 12 ? iv - 7 : iv > 4 ? iv - 2 : 2;
    if (adv_on || init_on) m = iv / 2 > 4 ? iv / 2 : 4;
    for (int k = 0; k < BLE_NCONN; k++) if (&conns[k] != C && conns[k].on) {
        int64_t d = (int64_t)(conns[k].c.anchor - cn.anchor) / 1250;    /* slots to the other link's event (either side of ours) */
        if (d < 0) d += 2 * conns[k].c.interval;
        if (d > 2 && (uint32_t)(d - 2) < m) m = (uint32_t)(d - 2);
    }
    return (uint16_t)m;
}
static void conn_arm(void)                       /* the window of event cn.evt, on its channel */
{
    EM16(CS1 + 0x16) = (uint16_t)((cn.csa2 ? csa2_ch(cn.evt) : csa1_next()) | (cn.master ? 0xCC00u : 0));      /* fixed channel this event (hardware hopping off); the IDF central keeps 0xCC00 in the upper byte */
    EM16(CS1 + 0x20) = maxev_slots();
    uint32_t w = cn.win / 2;                     /* RXWINCNTL in us (RXWIDE 0) */
    EM16(CS1 + 0x1A) = (uint16_t)(w > 0x7FFF ? 0x7FFF : w);
    if (cn.master) cn.anchor = (cn.anchor + 624u) / 625u * 625u;      /* the IDF central starts every event on a half-slot boundary (fine time 0x270): ours too */
    uint64_t st_hu = cn.anchor;
    uint32_t t = (uint32_t)(st_hu / 625u), f = cn.master ? 0x270u : 624u - (uint32_t)(st_hu % 625u);
    et_idx = (et_idx + 1) & 15;
    uint32_t e = 16 * et_idx;
    EM16(e + 0) = ble_conn_et[0] & ~0x38u;
    EM16(e + 2) = t & 0xFFFF; EM16(e + 4) = (t >> 16) & 0xFFF; EM16(e + 6) = f;
    EM16(e + 8) = CS1 / 2;
    EM16(e + 10) = cn.master ? 0x09F6 : ble_conn_et[5]; EM16(e + 12) = ble_conn_et[6]; EM16(e + 14) = ble_conn_et[7];
    CORE(0x24) = RING0;
    CORE(0x18) = 0xFFFFFFFFu;
    ev_c = C;
    CORE(0x100) = 0x80000000u | et_idx;
}

static void conn_start(const uint8_t *p, uint64_t adv_start_hu, uint8_t hdr0)   /* p: CONNECT_IND payload (34 bytes) */
{
    conn_t *nc = conn_free(); if (!nc) return;
    C = nc;
    last_conn = (uint8_t)(C - conns);
    cn.csa2 = (hdr0 >> 5) & 1;                   /* ChSel in the CONNECT_IND header: both sides support CSA#2 */
    const uint8_t *ll = p + 12;
    cn.aa = ll[0] | ll[1] << 8 | ll[2] << 16 | (uint32_t)ll[3] << 24;
    cn.crcinit = ll[4] | ll[5] << 8 | ll[6] << 16;
    uint8_t winsize = ll[7]; uint16_t winoff = ll[8] | ll[9] << 8;
    cn.interval = ll[10] | ll[11] << 8; cn.latency = ll[12] | ll[13] << 8; cn.timeout = ll[14] | ll[15] << 8;
    cn.hop = ll[21] & 0x1F; cn.last_unmapped = 0; cn.evt = 0; cn.upd_pending = cn.map_pending = cn.phy_pending = 0; cn.phy_tx = cn.phy_rx = 0;
    for (int k = 0; k < 45; k++) EM16(CS1 + 2 * k) = ble_conn_cs[k];
    EM16(CS1 + 0x0C) = cn.aa & 0xFFFF; EM16(CS1 + 0x0E) = cn.aa >> 16;
    EM16(CS1 + 0x10) = cn.crcinit & 0xFFFF; EM16(CS1 + 0x12) = cn.crcinit >> 16;
    EM16(CS1 + 0x1C) = 0;                        /* no TX descriptor: the core answers with empty PDUs */
    ring_n = RING_CN; EM16(CS1 + 0x28) = 27;   /* classic length until the central's LL_LENGTH_REQ */
    big_first = big_cnt = 0; tx_max_oct = 27;
    EM16(CS1 + 0x50) = 0;
    chm_apply(ll + 16);
    /* CONNECT_IND end: advert start + radio ramp (~100 us) + ADV_IND air + 150 us + CONNECT_IND air (352 us) */
    uint32_t adv_air = 8u * (1 + 4 + 2 + (EM16(TXD + 2) >> 8) + 3);
    uint64_t ci_end = adv_start_hu + 2u * (100 + adv_air + 150 + 352);
    uint64_t tw = ci_end + 2u * 1250u * (1u + winoff);          /* transmit window start */
    cn.anchor = tw - 2u * 200u;
    cn.win = 2u * (1250u * winsize + 400u);
    cn.last_rx_ms = ms_now();
    memcpy(init_addr, p, 6);
    peer_addr7[0] = (hdr0 >> 6) & 1; memcpy(peer_addr7 + 1, p, 6); own_addr7[0] = (hdr0 >> 7) & 1; memcpy(own_addr7 + 1, p + 6, 6);
    enc_state = tx_enc = rx_enc = mic_fail = enc_notify = 0; enc_block = 0; enc_tx_pend = 0;
    txq_head = txq_tail = 0;
    if (!adv_multi || !conn_free()) adv_on = 0;                  /* one more central may connect while a slot is free (ble_hw_adv_multi) */
    conn_on = 1; tx_first = tx_cnt = 0; big_first = big_cnt = 0;
}

/* Hand the queued PDUs to the core as one descriptor chain, as ROM r_lld_con_tx_prog does: every descriptor but the last carries the more-data bit and the
 * core sends them back to back inside one connection event. Only at event end (the core is idle) or while nothing is in flight. */
/* the CONNECT_IND went out after the matching ADV_IND (adv): start the master connection. The first packet goes out at the start of the transmit window (CONNECT_IND end +
 * 1.25 ms); the peripheral's window is WinSize wide and it resynchronises on that packet, so a few hundred microseconds of margin on our side do not matter. */
static void mconn_start(const ble_hw_pkt_t *adv)
{
    conn_t *nc = conn_free(); if (!nc) { init_on = 0; return; }
    C = nc;
    const uint8_t *ll = ic.ll;
    last_conn = (uint8_t)(C - conns);
    cn.master = 1; cn.csa2 = (adv->hdr0 >> 5) & 1;
    cn.aa = ll[0] | ll[1] << 8 | ll[2] << 16 | (uint32_t)ll[3] << 24; cn.crcinit = ll[4] | ll[5] << 8 | ll[6] << 16;
    cn.interval = ll[10] | ll[11] << 8; cn.latency = ll[12] | ll[13] << 8; cn.timeout = ll[14] | ll[15] << 8;
    cn.hop = ll[21] & 0x1F; cn.last_unmapped = 0; cn.evt = 0; cn.upd_pending = cn.map_pending = cn.phy_pending = 0; cn.phy_tx = cn.phy_rx = 0;
    for (int k = 0; k < 45; k++) EM16(CS1 + 2 * k) = ble_conn_cs[k];
    EM16(CS1) = 0x0002;                                                                       /* master connection format */
    for (int k = 0; k < 3; k++) EM16(CS1 + 6 + 2 * k) = ic.own[2 * k] | ic.own[2 * k + 1] << 8;
    EM16(CS1 + 0x0C) = cn.aa & 0xFFFF; EM16(CS1 + 0x0E) = cn.aa >> 16; EM16(CS1 + 0x10) = cn.crcinit & 0xFFFF; EM16(CS1 + 0x12) = cn.crcinit >> 16;
    EM16(CS1 + 0x14) = 0x0040; EM16(CS1 + 0x18) = 0x000B;
    for (int k = 0; k < 3; k++) EM16(CS1 + 0x2C + 2 * k) = ic.peer[2 * k] | ic.peer[2 * k + 1] << 8;       /* as the IDF central's connection CS: peer address, and the timing words of the master format */
    EM16(CS1 + 0x32) = 0; EM16(CS1 + 0x34) = IDLE; EM16(CS1 + 0x36) = 0x12; EM16(CS1 + 0x38) = 0x28; EM16(CS1 + 0x3A) = 0x12; EM16(CS1 + 0x3C) = 0x28;
    EM16(IDLE) = 0x8000; EM16(IDLE + 2) = 0x0001; EM16(IDLE + 4) = TXBUF;                  /* the idle descriptor: done, an empty PDU (the IDF central leaves its CS pointing at a finished descriptor) */
    EM16(CS1 + 0x1C) = IDLE; ring_n = RING_CN; EM16(CS1 + 0x28) = 27;
    big_first = big_cnt = 0; tx_max_oct = 27;
    EM16(CS1 + 0x50) = 0;
    chm_apply(ll + 16);
    uint64_t sync_hu = (uint64_t)adv->clkn * 625u;                                            /* half-us; the stamp is at the sync (end of the access address) */
    uint64_t adv_end = sync_hu + 2u * 8u * (2 + adv->len + 3);
    uint64_t ci_end = adv_end + 2u * (150 + 352);
    cn.anchor = ci_end + 2u * 1250u * (1u + 0x26u) + 2u * 2000u;                             /* WinOffset 0x24..0x26 on the air, window 10 ms wide: 3 ms into the latest one's window */
    cn.win = 2u * 30u; cn.last_rx_ms = ms_now();
    memcpy(init_addr, ic.peer, 6);
    peer_addr7[0] = ic.peer_rand; memcpy(peer_addr7 + 1, ic.peer, 6); own_addr7[0] = ic.own_rand; memcpy(own_addr7 + 1, ic.own, 6);
    enc_state = tx_enc = rx_enc = mic_fail = enc_notify = 0; enc_block = 0; enc_tx_pend = 0;
    txq_head = txq_tail = 0;
    adv_on = 0; init_on = 0; conn_on = 1; tx_first = tx_cnt = 0;
}

static int live;                                 /* tx_load runs inside a connection event: the core owns the chain pointer */
static void tx_reap(void)
{
    while (tx_cnt && ((EM16(TXDSC(tx_first)) & 0x8000) || !EM16(CS1 + 0x1C))) {      /* acknowledged: done bit, or the core's pointer is clear */
        if (desc_big[tx_first]) { big_first = (big_first + 1) % TXB_N; big_cnt--; }
        tx_first = (tx_first + 1) % TXD_N; tx_cnt--; st.tx_acked++;
    }
    if (!tx_cnt && !live) EM16(CS1 + 0x1C) = cn.master ? IDLE : 0;
}
static void tx_load(void)
{
    tx_reap();
    while (tx_cnt < TXD_N && txq_tail != txq_head && (tx_cnt || !live)) {
        if (live) {                                                                  /* inside an event only behind a descriptor the core has not reached: once it finished the last one its pointer is clear, and */
            uint16_t cur = EM16(CS1 + 0x1C);                                         /* a descriptor linked behind that is never sent (and conn_next would count it as acknowledged) */
            if (!cur || cur == TXDSC((tx_first + tx_cnt - 1) % TXD_N)) break;
        }
        unsigned l = txq_at(1), llid = txq_at(0), i = (tx_first + tx_cnt) % TXD_N, big = l > 27;
        if (big && big_cnt == TXB_N) break;                                          /* all long slots busy: next event */
        unsigned b = (big_first + big_cnt) % TXB_N, ptr = big ? TXB(b) : TXDBUF(i);
        uint8_t *dst = (uint8_t *)tx_buf + (ptr - TXBUF);
        for (unsigned k = 0; k < l; k++) dst[k] = txq_at(2 + k);
        EM16(TXDSC(i)) = 0; EM16(TXDSC(i) + 2) = (uint16_t)((l && tx_enc ? l + 4 : l) << 8 | llid);       /* encrypted: the length field includes the 4-byte MIC the core adds */ EM16(TXDSC(i) + 4) = (uint16_t)ptr;
        if (big) big_cnt++;
        desc_big[i] = (uint8_t)big;
        if (tx_cnt) { unsigned p = (i + TXD_N - 1) % TXD_N; EM16(TXDSC(p)) = TXDSC(i); EM16(TXDSC(p) + 2) |= 0x10; }
        else EM16(CS1 + 0x1C) = TXDSC(i);
        tx_cnt++; txq_tail = (txq_tail + 2 + l) % TXQ_BYTES; st.tx_pdus++;
    }
}

static void arm_adv(void);
static void conn_end(void)                       /* link lost or terminated: back to advertising */
{
    conn_on = 0; ring_n = nconn_on() ? RING_CN : RING_N;
    enc_state = tx_enc = rx_enc = enc_notify = enc_block = enc_tx_pend = 0;
    st.disconnects++;
    for (int e = 0; e < 16; e++) EM16(16 * e + 0) = 0x281A;
    if (cn.master) { cn.master = 0; return; }                    /* a central link ends in silence, the application decides what next */
    adv_on = 1;
    adv_next = clkn() + 16;
}

/* after each connection event: next anchor, pending updates at their instant, supervision */
/* anchor tracking: after an event with a PDU, the next window opens SYNC_LEAD before the expected sync
 * (last sync + interval) and lasts 2 * SYNC_LEAD; without a PDU it widens by WIDEN per missed event. */
#define SYNC_LEAD (2u * 300u)                    /* half-us */
#define WIDEN     (2u * 50u)
static void conn_catchup(void)
{
    uint64_t now = now_hu();
    while ((int64_t)(cn.anchor - now) < 2 * 1000) {        /* never schedule into the past: skip whole events */
        cn.anchor += 2u * 1250u * cn.interval; cn.evt++; if (!cn.csa2) csa1_next();
    }
}
static void conn_next(int got, uint64_t sync)
{
    tx_reap();
    if (enc_tx_pend && !tx_cnt) { EM16(CS1 + 2) |= 0x200; tx_enc = 1; enc_tx_pend = 0; enc_block = 0; }      /* the core is idle and nothing is in flight: encryption on, and the first encrypted PDU is loaded before the next event */
    tx_load();
    if (got) cn.last_rx_ms = ms_now();
    if (ms_now() - cn.last_rx_ms > cn.timeout * 10u) { conn_end(); return; }
    cn.evt++;
    EM16(CS1 + 0x50) = cn.evt;
    uint64_t iv = 2u * 1250u * cn.interval;
    if (cn.master) cn.anchor += iv;                              /* the central owns the anchor: events follow at the interval */
    else if (cn.upd_pending && cn.evt == cn.instant_upd) {           /* connection update: new window at the instant */
        cn.upd_pending = 0;
        cn.anchor += iv + 2u * 1250u * cn.new_winoff;
        cn.interval = cn.new_interval; cn.latency = cn.new_latency; cn.timeout = cn.new_timeout;
        cn.win = 2u * (1250u * cn.new_winsize + 400u);
    } else if (got && sync && cn.evt > 1 &&
               (int64_t)(sync + iv - SYNC_LEAD - (cn.anchor + iv)) < (int64_t)cn.win &&
               (int64_t)((cn.anchor + iv) - (sync + iv - SYNC_LEAD)) < (int64_t)cn.win) {   /* plausible sync only */
        last_sync = sync; missed = 0;
        cn.anchor = sync + iv - SYNC_LEAD;
        cn.win = 2u * SYNC_LEAD;
    } else {
        missed++;
        cn.anchor += iv - WIDEN / 2; cn.win += WIDEN;
        if (cn.win > 2u * 1250u * 4u) cn.win = 2u * 1250u * 4u;
    }
    if (cn.map_pending && cn.evt == cn.instant_map) { cn.map_pending = 0; chm_apply(cn.new_chm); }
    if (cn.phy_pending && cn.evt == cn.instant_phy) {            /* PHY update: ROM r_lld_con_evt_start_cbk writes the new PHYs to CS+4, bits 1:0 transmit, 3:2 receive */
        cn.phy_pending = 0; cn.phy_tx = cn.new_tx_phy; cn.phy_rx = cn.new_rx_phy;
        EM16(CS1 + 4) = (uint16_t)((EM16(CS1 + 4) & ~0xFu) | cn.phy_tx | cn.phy_rx << 2);
    }
    conn_catchup();
}

static uint8_t raw_on;
uint32_t ble_hw_raw_jitter;
uint32_t ble_hw_raw_lead = 400;                /* raw event kick lead, half-us: 200 us receives 1000/1000 at 740 packets/s, 250 us misses 9 %, <= 150 us is never picked up (bench/rfmeas/rx_lead.txt) */
static volatile uint32_t raw_left;             /* raw send: packets still to send, 0 = continuous */
static uint16_t raw_cs = CS0;
uint8_t ble_hw_raw_fmt = 0x02, ble_hw_raw_tmpl;   /* bench: CS format / template (0 conn, 1 adv) for ble_hw_raw_send */
uint16_t ble_hw_raw_t18 = 0xFFFF, ble_hw_rx_set_off, ble_hw_rx_set_val;
uint32_t ble_hw_rx_set_val32;
uint8_t ble_hw_rx_libphy, ble_hw_rx_big = 1, ble_hw_rx_fmt = 3;   /* receive: format 0x03 takes up to 255 bytes; 2 own buffers */
uint16_t ble_hw_rx_t18 = 0xE000;
static unsigned raw_rx_n = RING_N;
uint32_t ble_hw_raw_flags;                     /* bit 0: CRC off, bit 1: whitening off */
uint16_t ble_hw_tx_flip_off, ble_hw_tx_flip_bits;   /* bench: XOR into the send CS */
/* RWBLECNTL on this core (measured, bench/rfmeas/rf_pkt_test.txt): bit 13 = CRC off (TX sends no valid CRC, RX ignores it),
 * bit 14 = whitening off (header, length and payload sent / taken literally; bench/rfmeas/whiten_b14.txt),
 * bit 19 = the header byte's bits 2..4 are not rewritten (sequence numbers off), bit 9 = advertising filter (37-byte cap).
 * Bits 17 / 18 (whitening / CRC on other RW cores) change nothing here. */
static void raw_flags(int tx)                 /* bit 19 on the receiver loses packets (measured): transmitter only */
{
    CORE(0x00) = (CORE(0x00) & ~(1u << 13 | 1u << 14 | 1u << 9 | 1u << 19)) | (ble_hw_raw_flags & 1u) << 13 | (ble_hw_raw_flags & 2u) << 13
                | (tx ? 1u << 19 : 0);
}
uint32_t ble_hw_raw_d0, ble_hw_raw_poke_off, ble_hw_raw_poke_or;   /* bench: extra bits before the next raw TX kick */
static void raw_kick(void);
/* Arm the next event. Links first, by window: a scan window (12.8 ms) or an advert (about 4 ms) only runs when it ends before the earliest window of a link. */
static void sched(void)
{
    conn_t *b = 0;
    for (int k = 0; k < BLE_NCONN; k++) if (conns[k].on && (!b || (int64_t)(conns[k].c.anchor - b->c.anchor) < 0)) b = &conns[k];
    if (!b) { if (init_on) init_arm(); else if (adv_on) arm_adv(); return; }
    int64_t gap = (int64_t)(b->c.anchor - now_hu());
    if (init_on && gap > 2 * 16000) { init_arm(); return; }
    if (adv_on) {
        int64_t at = (int64_t)((uint64_t)adv_next * 625u) - (int64_t)now_hu();
        if (at < 2 * 625 * 4) at = 2 * 625 * 4;                  /* arm_adv starts no earlier than 4 slots from now */
        if (at + 2 * 4000 < gap) { arm_adv(); return; }
    }
    C = b; conn_catchup(); conn_arm();
}
static void service_(void)
{
    if (raw_on) {
        last_end = ccount();
        CORE(0x18) = 0xFFFFFFFFu;
        if (raw_on == 2) {                       /* send the same buffer again, or stop after the requested count */
            st.events++;
            if (raw_left && !--raw_left) { raw_on = 0; CORE(0x0C) = INT_END | INT_SKIP; return; }
            C = conns; EM16(TXC) = 0; EM16(CS1 + 0x1C) = TXC;
        }
        raw_kick(); CORE(0x0C) = INT_END | INT_SKIP; return;
    }
    uint32_t raw = CORE(0x14);
    if ((raw & INT_RX) && !(raw & (INT_END | INT_SKIP))) {      /* PDU inside a connection event: refill the TX chain so a long burst is not cut at its length */
        CORE(0x18) = INT_RX;
        if (ev_c && !raw_on) { C = ev_c; live = 1; tx_load(); live = 0; }
        CORE(0x0C) = BLE_MASK | INT_RX; return;
    }
#ifdef BLE_RX_ACK
    if ((raw & (1u << 2)) && !(raw & (INT_END | INT_SKIP))) { CORE(0x18) = 1u << 2; drain(); }   /* PDU inside a long window */
#endif
    if (raw & (INT_END | INT_SKIP)) {
        uint32_t c0 = ccount();
        CORE(0x18) = raw;
        for (int k = 0; k < 32; k++) {           /* ESP interrupt FIFO 0x2D8: [9:5] pending, write bit 0 to pop (as ROM r_rwble_isr) */
            uint32_t f = CORE(0x2D8);
            if (!((f >> 5) & 0x1F)) break;
            CORE(0x2D8) = f | 1;
        }
        st.events++; init_stuck = 0;
        if (raw & INT_SKIP) st.skips++;
        last_end = c0;
        if (ev_c) C = ev_c;                      /* the connection whose event ended */
        const int ec = ev_c != 0;
        if (ec && term_tx) { tx_reap(); if (!tx_cnt && txq_tail == txq_head) { term_tx = 0; drain(); conn_end(); goto ended; } }
        if (ec && (term_req || mic_fail)) { term_req = mic_fail = 0; drain(); conn_end(); }
        else if (ec) {
            uint32_t before = st.packets;
            uint64_t sync = 0;                     /* first PDU of the event: CLKN + fine count at sync (RXCLKNSYNC, RXFCNTSYNC[9:0]) */
            uint32_t best = 0xFFFFFFFF;
            for (uint32_t d = RING0; d < RING0 + ring_n * 0x14; d += 0x14) if ((EM16(d) & 0x8000) && !(EM16(d + 2) & 0x000F)) {
                uint32_t ck = EM16(d + 8) | (uint32_t)EM16(d + 10) << 16;
                if (ck < best) { best = ck; sync = (uint64_t)ck * 625u + (624u - (EM16(d + 0x0C) & 0x3FF)); }
            }
            drain(); conn_next(st.packets != before, sync);
        }
        else if (init_on) {
            uint32_t h0 = q_head; drain();
            int hit = -1;
            for (uint32_t k = h0; k != q_head; k = (k + 1) % Q_N) if ((q[k].hdr0 & 0xF) == 0 && q[k].len >= 6 && !memcmp(q[k].data, ic.peer, 6)) hit = (int)k;
            if (hit >= 0 && (EM16(TXD) & 0x8000)) mconn_start(&q[hit]);
            else if (ms_now() - ic.t_ms > 15000) init_on = 0;
        }
        else if (adv_on) {
            uint64_t adv_start = (uint64_t)adv_next * 625u;          /* this advert started at adv_next (fine 0x270 = slot start) */
            uint32_t h0 = q_head; drain();
            int connected = 0;
            for (uint32_t k = h0; k != q_head; k = (k + 1) % Q_N)   /* CONNECT_IND to our address? */
                if ((q[k].hdr0 & 0xF) == 5 && q[k].len == 34 && !memcmp(q[k].data + 6, own_addr, 6)) { conn_start(q[k].data, adv_start, q[k].hdr0); connected = 1; break; }
            if (!connected) adv_next += adv_iv;
        }
        else if (scan_chan) arm(); else drain();
    ended:
        if (nconn_on() || init_on || adv_on) sched();
        st.isr_cycles += ccount() - c0;
    }
    CORE(0x0C) = BLE_MASK | (nconn_on() ? INT_RX : 0);
}

/* init steps: 1 LO table, 2 modem clocks / resets released, 4 coex grant, 8 BT sequencer, 16 modem, 32 EM + core */
static void init_steps(uint32_t m)
{
    if (m & 1) rf_freq_mem_load();               /* per-MHz LO table the core tunes from */
    if (m & 2) {
    R32(0x60026014) = 0xFFFFDFDFu;               /* modem clocks incl. BT (SYSCON) */
    R32(0x60026018) = 0;
    }
    if (m & 4)
    for (unsigned k = 0; k < sizeof ble_vendor_phy / sizeof ble_vendor_phy[0]; k++) {
        uint32_t a = ble_vendor_phy[k].addr;     /* coex arbiter: grant the radio to BT (page 0; 3 mirrors) */
        if (a >= 0x60035000u && a < 0x60035400u && a != 0x60035000u && a != 0x6003507Cu) R32(a) = ble_vendor_phy[k].val;
    }
    /* BT baseband / analog-bus sequencer 0x6000E000: loaded while E0C4 bit 25 is held (libbtbb does the same) */
    if (m & 8) {
    R32(0x6000E0C4) |= 1u << 25;
    uint32_t c4 = 0;
    for (unsigned k = 0; k < sizeof ble_vendor_phy / sizeof ble_vendor_phy[0]; k++) {
        uint32_t a = ble_vendor_phy[k].addr;
        if (a == 0x6000E0C4u) c4 = ble_vendor_phy[k].val;
        else if (a >= 0x6000E000u && a < 0x6000E200u) R32(a) = ble_vendor_phy[k].val;
    }
    R32(0x600060FC) = (R32(0x600060FC) & 0xFFFF00FFu) | 0xC800u;
    R32(0x6000E0C4) = c4 & ~(1u << 25);
    }
    if (m & 16)
    for (int k = 0; k < 64; k++) {               /* 0x6C..0x88, 0xB0 are run-time state (captured mid-scan): leave them */
        if ((k >= 0x6C / 4 && k <= 0x88 / 4) || k == 0xB0 / 4) continue;
        R32(0x60011000u + 4 * k) = ble_modem_regs[k];
    }
    if (!(m & 32)) return;
    memcpy(em_buf, ble_em_image, sizeof em_buf);
    for (int k = 0; k < 56; k++) R32(0x60031204u + 4 * k + (k > 47 ? 28 : 0)) = ble_em_map_off[k];   /* offset-only entries */
    for (unsigned k = 0; k < sizeof ble_em_segs / sizeof ble_em_segs[0]; k++) {
        unsigned i = ble_em_segs[k].idx;
        uint32_t a = (uint32_t)em_buf + ble_em_segs[k].pos;
        R32(0x60031204u + 4 * i + (i > 47 ? 28 : 0)) = ((uint32_t)ble_em_segs[k].em_off >> 2) << 18 | ((a >> 2) & 0x3FFFF);
    }
    {   /* RX ring of RING_CN descriptors in its own page; payloads of the extra ones in rxpay_buf (two pages at RXPAY) */
        uint32_t rpos = 0, tmpl[5];
        for (unsigned k = 0; k < sizeof ble_em_segs / sizeof ble_em_segs[0]; k++) if (ble_em_segs[k].em_off == 0x1000) rpos = ble_em_segs[k].pos;
        memcpy(ring_buf, (const uint8_t *)em_buf + rpos, 10 * 0x14);
        memcpy(tmpl, ring_buf, sizeof tmpl);
        for (unsigned k = 0; k < RING_CN; k++) {
            uint16_t *dd = (uint16_t *)((uint8_t *)ring_buf + k * 0x14);
            if (k >= RING_N) { memcpy(dd, tmpl, 0x14); dd[9] = rx_pay(k); }
            dd[0] = (uint16_t)(k + 1 < RING_CN ? 0x1000 + (k + 1) * 0x14 : 0x1000); dd[1] = 0x9000; dd[2] = 0;
        }
        R32(0x60031204u + 4 * 3) = (0x1000u >> 2) << 18 | (((uint32_t)ring_buf >> 2) & 0x3FFFF);
        static const uint8_t rxpage[2] = { 13, 14 };             /* page-map entries for the RX payload region (entries 8 and 11 do not work as payload pages: the link dies in transmit, measured) */
        for (unsigned k = 0; k < 2; k++) R32(0x60031204u + 4 * rxpage[k]) = ((RXPAY + 0x400u * k) >> 2) << 18 | ((((uint32_t)rxpay_buf + 0x400u * k) >> 2) & 0x3FFFF);
    }
    for (unsigned k = 0; k < sizeof ble_core_regs / sizeof ble_core_regs[0]; k++) CORE(ble_core_regs[k].off) = ble_core_regs[k].val;
    R32(0x60031204u + 4 * 7) = (TXBUF >> 2) << 18 | (((uint32_t)tx_buf >> 2) & 0x3FFFF);   /* EM pages for the TX payloads: 2 KB at TXBUF */
    { static const uint8_t txpage[3] = { 10, 9, 12 };                  /* entries 7 and these three: EM 0x2400..0x33FF */                    /* ... the 4 KB TX payload region continues in three more pages */
      for (unsigned k = 0; k < 3; k++) R32(0x60031204u + 4 * txpage[k]) = ((TXBUF + 0x400u * (k + 1)) >> 2) << 18 | ((((uint32_t)tx_buf + 0x400u * (k + 1)) >> 2) & 0x3FFFF); }
    R32(0x60031204u + 4 * 8) = (0x4800u >> 2) << 18;                /* the vendor's entry 8 still says 0x2C00, now entry 9's: keep offsets unique, page invalid. (Entries 8 and 11 as payload pages broke the link, measured; entry 11 is not touched at all.) */
    CORE(0x2C4) |= 1u << 7 | 1u << 9 | 1u << 10 | 1u << 12 | 1u << 13 | 1u << 14;                      /* page-valid mask (pages 0..31; 0x2C8: 32..47, 0x300: 48..): the core
                                                  * ignores a mapped page whose bit is clear (vendor: r_emi_alloc_em_mapping) */
    for (int k = 0; k < 45; k++) scan_cs[k] = EM16(CS0 + 2 * k);
    CORE(0x0C) = 0;
    CORE(0x00) = ble_rwblecntl;
    for (uint32_t d = RING0; d < RING0 + RING_N * 0x14; d += 0x14) { EM16(d) &= 0x7FFF; EM16(d + 4) = 0; }
    CORE(0x18) = 0xFFFFFFFFu;
    CORE(0x0C) = INT_END | INT_SKIP;
}
int ble_hw_init(void)
{
    conns_init();
    init_steps(0xFF);
    return irq_attach(SRC_RWBLE, 1, isr, 0) < 0 ? -1 : 0;
}
/* Link-layer core reset (SYSCON WIFI_RST_EN bit 9, RW_BTMAC_RST) and reload of modem + core. A coded-rate receive
 * window that times out while the AGC runs leaves the core unable to sync on any rate (events still end, status 0x01)
 * until this reset; rf_open, ble_hw_init, the watchdog abort or any single AGC / modem / core word do not clear it
 * (bench/rfmeas/data/coded_revive*.txt). */
void ble_hw_core_reset(void)
{
    uint32_t ps = irq_lock();
    raw_on = 0;
    R32(0x60026018) |= 1u << 9;
    R32(0x60026018) &= ~(1u << 9);
    init_steps(16 | 32);
    irq_unlock(ps);
}

void ble_hw_scan(uint8_t chan)
{
    adv_on = 0;
    for (int k = 0; k < 45; k++) EM16(CS0 + 2 * k) = scan_cs[k];
    scan_chan = chan;
    last_end = ccount();
    arm();
}

void ble_hw_stop(void) { scan_chan = 0; adv_on = 0; conns_off(); }

int ble_hw_connected(void) { return nconn_on() != 0; }
int ble_hw_conn_count(void) { return nconn_on(); }
int ble_hw_link_up(int h) { return h >= 0 && h < BLE_NCONN && conns[h].on; }
int ble_hw_select(int h) { if (h < 0 || h >= BLE_NCONN) return -1; sel = (uint8_t)h; return 0; }
int ble_hw_selected(void) { return sel; }
int ble_hw_last_conn(void) { return last_conn; }                        /* the link the latest connection came up on */
unsigned ble_hw_tx_max(void) { C = &conns[sel]; return tx_max_oct; }
void ble_hw_conn_addrs(uint8_t peer[7], uint8_t own[7]) { C = &conns[sel]; memcpy(peer, peer_addr7, 7); memcpy(own, own_addr7, 7); }
void ble_hw_phy(uint8_t *tx, uint8_t *rx) { C = &conns[sel]; *tx = cn.phy_tx; *rx = cn.phy_rx; }
int ble_hw_conn_info(uint8_t *csa2, uint16_t *interval) { C = &conns[sel]; *csa2 = cn.csa2; *interval = cn.interval; return conn_on; }

/* queue one LL data PDU (llid 2: L2CAP start, 3: LL control); -1 while the previous one is not acknowledged */
int ble_hw_tx_free(void) { unsigned mx = ble_hw_tx_max(); return (int)(((txq_tail + TXQ_BYTES - txq_head - 1) % TXQ_BYTES) / (mx + 2)); }   /* PDUs of the largest size that still fit the TX queue */

static int send_c(uint8_t llid, const uint8_t *data, unsigned len)       /* on C */
{
    if (!conn_on || len > tx_max_oct || (enc_block && llid != 3)) return -1;
    uint32_t ps = irq_lock();
    if (len + 2 > (txq_tail + TXQ_BYTES - txq_head - 1) % TXQ_BYTES) { irq_unlock(ps); return -1; }     /* queue full */
    uint32_t h = txq_head;
    txq_buf[h] = llid; h = (h + 1) % TXQ_BYTES; txq_buf[h] = (uint8_t)len; h = (h + 1) % TXQ_BYTES;
    for (unsigned k = 0; k < len; k++) { txq_buf[h] = data[k]; h = (h + 1) % TXQ_BYTES; }
    txq_head = h;
    /* a chain in flight is extended only at event end (or by the RX interrupt inside the event). An empty chain is started here only while the core is idle: inside an event the core finishes its last
     * descriptor by writing the chain pointer, which would overwrite the one set here and strand the new chain (the write response of a burst start was lost this way) */
    if (!tx_cnt && !enc_tx_pend && (int64_t)(cn.anchor - now_hu()) > (int64_t)cn.win + 2 * 3000) tx_load();
    irq_unlock(ps);
    return 0;
}
int ble_hw_send(uint8_t llid, const uint8_t *data, unsigned len) { C = &conns[sel]; return send_c(llid, data, len); }

/* LL control the link layer must answer; the rest goes to the application. Returns 1 if consumed. */
int ble_hw_ll_control(const ble_hw_pkt_t *p)
{
    if ((p->hdr0 & 3) != 3 || !p->len || p->conn >= BLE_NCONN) return 0;
    C = &conns[p->conn];
    const uint8_t *d = p->data; uint8_t r[27];
    switch (d[0]) {
    case 0x00:                                   /* LL_CONNECTION_UPDATE_IND: WinSize WinOffset Interval Latency Timeout Instant */
        cn.new_winsize = d[1]; cn.new_winoff = d[2] | d[3] << 8; cn.new_interval = d[4] | d[5] << 8;
        cn.new_latency = d[6] | d[7] << 8; cn.new_timeout = d[8] | d[9] << 8; cn.instant_upd = d[10] | d[11] << 8; cn.upd_pending = 1;
        return 1;
    case 0x01:                                   /* LL_CHANNEL_MAP_IND */
        memcpy(cn.new_chm, d + 1, 5); cn.instant_map = d[6] | d[7] << 8; cn.map_pending = 1; return 1;
    case 0x02: if (conn_on) term_req = 1; return 1;                  /* LL_TERMINATE_IND: ended at the next event end (not from here: the core may have an event armed) */
    case 0x08: case 0x0E:                        /* LL_FEATURE_REQ / LL_PERIPHERAL_FEATURE_REQ -> LL_FEATURE_RSP: LE encryption (byte 0 bit 0), extended reject (2), LE ping (4), data length extension (5); LE 2M PHY (byte 1 bit 0) */
        r[0] = 0x09; memset(r + 1, 0, 8); r[1] = 0x35; r[2] = 0x01; send_c(3, r, 9); return 1;
    case 0x16: r[0] = 0x17; r[1] = 0x03; r[2] = 0x03; send_c(3, r, 3); return 1;       /* LL_PHY_REQ -> LL_PHY_RSP: 1M and 2M both ways; the central decides in LL_PHY_UPDATE_IND */
    case 0x18:                                   /* LL_PHY_UPDATE_IND: PHY central -> us, us -> central (0 = unchanged), instant */
        if (d[1] || d[2]) {
            cn.new_rx_phy = d[1] ? (d[1] & 2 ? 1 : 0) : cn.phy_rx; cn.new_tx_phy = d[2] ? (d[2] & 2 ? 1 : 0) : cn.phy_tx;
            cn.instant_phy = d[3] | d[4] << 8; cn.phy_pending = 1;
        }
        return 1;
    case 0x0C:                                   /* LL_VERSION_IND: 5.0, Espressif (0x02E5) */
        r[0] = 0x0C; r[1] = 9; r[2] = 0xE5; r[3] = 0x02; r[4] = 0; r[5] = 0; send_c(3, r, 6); return 1;
    case 0x12: r[0] = 0x13; send_c(3, r, 1); return 1;     /* LL_PING_REQ */
    case 0x14: {                                 /* LL_LENGTH_REQ: the peer's max receive octets d[1..2] set how long a PDU we may send. We send up to 251 (256-byte payload slots) but receive classic
                                                  * 27-byte PDUs only: the RX ring buffers hold 27 bytes, and a GATT server's traffic toward the central is the big direction. */
        unsigned prx = d[1] | d[2] << 8;
        tx_max_oct = (uint16_t)(prx < 27 || !C->nb ? 27 : prx > 251 ? 251 : prx);        /* only link 0 has the long-PDU slots */
        r[0] = 0x15; r[1] = 27; r[2] = 0; r[3] = 0x48; r[4] = 0x01; r[5] = 251; r[6] = 0; r[7] = 0x48; r[8] = 0x08;       /* LL_LENGTH_RSP: receive 27 octets / 328 us, transmit 251 / 2120 us */
        send_c(3, r, 9); return 1;
    }
    case 0x03: {                                 /* LL_ENC_REQ: Rand(8) EDIV(2) SKDm(8) IVm(4) -> LL_ENC_RSP (SKDs, IVs), then LL_START_ENC_REQ */
        uint8_t ltk[16], skd[16], iv[8], sk[16], key[16], pt[16];
        if (p->len != 23 || enc_state || !enc_ltk_cb || enc_ltk_cb(d + 1, (uint16_t)(d[9] | d[10] << 8), ltk)) {
            r[0] = 0x11; r[1] = 0x03; r[2] = 0x06; send_c(3, r, 3); return 1;                  /* LL_REJECT_EXT_IND: PIN or key missing */
        }
        memcpy(skd, d + 11, 8); rng_fill(skd + 8, 8); memcpy(iv, d + 19, 4); rng_fill(iv + 4, 4);       /* SKD = SKDm | SKDs, IV = IVm | IVs, least significant first */
        for (int k = 0; k < 16; k++) { key[k] = ltk[15 - k]; pt[k] = skd[15 - k]; }
        aes_block(key, 16, true, pt, sk);                                                         /* SK = e(LTK, SKD) (most significant octet first) */
        uint32_t ps = irq_lock();
        for (int k = 0; k < 8; k++) {
#ifdef BLE_SK_LE
            EM16(CS1 + 0x2C + 2 * k) = (uint16_t)(sk[15 - 2 * k] | sk[14 - 2 * k] << 8);
#else
            EM16(CS1 + 0x2C + 2 * k) = (uint16_t)(sk[2 * k] | sk[2 * k + 1] << 8);
#endif
        }
        for (int k = 0; k < 4; k++) EM16(CS1 + 0x3C + 2 * k) = (uint16_t)(iv[2 * k] | iv[2 * k + 1] << 8);
        for (int k = 0; k < 6; k++) EM16(CS1 + 0x44 + 2 * k) = 0;
        EM16(CS1 + 2) |= 0x100; rx_enc = 1; enc_state = 1;
        irq_unlock(ps);
        r[0] = 0x04; memcpy(r + 1, skd + 8, 8); memcpy(r + 9, iv + 4, 4); send_c(3, r, 13);
        r[0] = 0x05; send_c(3, r, 1); enc_block = 1; return 1;       /* from here only LL control until the encryption runs: the central expects every PDU of ours encrypted */
    }
    case 0x06:                                   /* LL_START_ENC_RSP (received decrypted): encrypt our direction and answer */
        if (enc_state != 1) return 1;
        enc_tx_pend = 1; enc_state = 2; enc_notify = 1;                                       /* the host hears of it (enc_done_cb) once the encrypted direction runs, ble_hw_read */
        r[0] = 0x06; send_c(3, r, 1);
        return 1;
    case 0x0A: r[0] = 0x11; r[1] = 0x0A; r[2] = 0x1A; send_c(3, r, 3); return 1;       /* LL_PAUSE_ENC_REQ: a new key mid-link is not supported */
    case 0x07: case 0x09: case 0x0D: case 0x13: case 0x15: return 1;   /* responses / reject: nothing to do */
    default: r[0] = 0x07; r[1] = d[0]; send_c(3, r, 2); return 1; /* LL_UNKNOWN_RSP */
    }
}

void ble_hw_set_adva(const uint8_t adva[6])      /* new advertiser address while advertising: the core reads it at the start of each event (a rotation does not restart the advertising) */
{
    uint32_t ps = irq_lock();
    for (int k = 0; k < 3; k++) EM16(CS0 + 6 + 2 * k) = adva[2 * k] | adva[2 * k + 1] << 8;
    memcpy(own_addr, adva, 6);
    irq_unlock(ps);
}
int ble_hw_adv(uint8_t chan, const uint8_t adva[6], int random, const uint8_t *data, unsigned len, uint32_t interval_us)
{
    if (len > 31) return -1;
    uint32_t ps = irq_lock();

    scan_chan = 0;
    for (int k = 0; k < 45; k++) EM16(CS0 + 2 * k) = ble_adv_cs[k];
    EM16(CS0 + 0x16) = chan;                     /* fixed channel, no hop */
    for (int k = 0; k < 3; k++) EM16(CS0 + 6 + 2 * k) = adva[2 * k] | adva[2 * k + 1] << 8;
    memcpy(own_addr, adva, 6);
    /* random own address: the vendor sets LINKCNTL bit 7, TxAdd in the header and bit 6 of TX descriptor word 8 */
    EM16(CS0 + 2) = (EM16(CS0 + 2) & ~0x80u) | (random ? 0x80 : 0);
    for (int k = 0; k < 9; k++) EM16(TXD + 2 * k) = ble_adv_txdesc[k];
    EM16(0x14C) = ble_adv_em14c;
    EM16(TXD + 2) = (uint16_t)((len + 6) << 8 | (adv_csa2 || adv_type ? 0x20 : 0) | adv_type | (random ? 0x40 : 0));   /* 0x20 = ChSel: we support CSA#2 */
    EM16(TXD + 4) = TXBUF;
    EM16(TXD + 16) = (EM16(TXD + 16) & ~0x40u) | (random ? 0x40 : 0);
    memcpy(tx_buf, data, len);
    for (unsigned k = 0; k < sizeof ble_adv_core_regs / sizeof ble_adv_core_regs[0]; k++)   /* core as the vendor had it advertising */
        if (ble_adv_core_regs[k].off >= 0x2C0) CORE(ble_adv_core_regs[k].off) = ble_adv_core_regs[k].val;
    adv_iv = interval_us * 2 / 625;              /* half-slots */
    if (!adv_iv) adv_iv = 1;
    for (int e = 0; e < 16; e++) {               /* every exchange-table entry valid, as the vendor leaves them */
        EM16(16 * e + 0) = 0x281A; EM16(16 * e + 2) = 0; EM16(16 * e + 4) = 0; EM16(16 * e + 6) = 0x270;
        EM16(16 * e + 8) = CS0 / 2; EM16(16 * e + 10) = 0x030B; EM16(16 * e + 12) = 0x0C00; EM16(16 * e + 14) = 0x0F00;
    }
    adv_on = 1;
    adv_next = clkn() + 16;
    last_end = ccount();
    arm_adv();
    irq_unlock(ps);
    return 0;
}

int ble_hw_read(ble_hw_pkt_t *p)
{
    for (int k = 0; k < BLE_NCONN; k++) {         /* main-loop context: the host may queue its identity keys now */
        conn_t *sv = C; C = &conns[k];
        if (enc_notify && tx_enc) { enc_notify = 0; if (enc_done_cb) { uint8_t ss = sel; sel = (uint8_t)k; enc_done_cb(1); sel = ss; } }
        C = sv;
    }
    service();
    if (scan_chan && !adv_on && ccount() - last_end > 240000u * 50) {   /* no event end for 50 ms: re-arm (a lost start) */
        uint32_t ps = irq_lock();
        CORE(0x00) |= 1u << 24;                  /* SCAN_ABORT: a stuck event refuses new starts */
        for (volatile int k = 0; k < 200; k++) {}
        CORE(0x00) &= ~(1u << 24);
        st.aborts++;
        last_end = ccount(); arm(); irq_unlock(ps);
    }
    if (init_on && !nconn_on() && ccount() - last_end > 240000u * 50) {   /* initiating alone: a window ends every 12.8 ms; none for 50 ms means a stuck event (a retry after a timed-out attempt gets there), abort and start again */
        uint32_t ps = irq_lock();
        CORE(0x00) |= 1u << 24;
        for (volatile int k = 0; k < 200; k++) {}
        CORE(0x00) &= ~(1u << 24);
        st.aborts++;
        if (++init_stuck >= 3) { init_stuck = 0; ble_hw_core_reset(); init_setup(); }          /* the abort did not free it: reset the whole core (some boots hang here after a few dozen windows) */
        last_end = ccount(); init_arm(); irq_unlock(ps);
    }
    if (q_tail == q_head) return 0;
    *p = q[q_tail];
    q_tail = (q_tail + 1) % Q_N;
    return 1;
}

void ble_hw_get_stats(ble_hw_stats_t *s) { *s = st; }

static void wake(void *arg) { (void)arg; }
int ble_hw_wait(ble_hw_pkt_t *p, uint32_t timeout_us)
{
    static swtimer_t to;                         /* wakes waiti at the timeout when no BLE interrupt comes */
    uint32_t t0 = ccount();
    swt_start(&to, timeout_us, false, wake, 0);
    int r = 0;
    for (;;) {
        if (ble_hw_read(p)) { r = 1; break; }
        if (ccount() - t0 >= timeout_us * 240u) break;
        __asm__ volatile("waiti 0");
    }
    swt_stop(&to);
    return r;
}

void ble_hw_debug(uint32_t out[6])
{
    uint32_t e = 16 * et_idx;
    out[0] = CORE(0x0C); out[1] = CORE(0x14); out[2] = EM16(e) | (uint32_t)EM16(CS1 + 0x1A) << 16; out[3] = clkn(); out[4] = EM16(e + 2) | (uint32_t)EM16(e + 4) << 16;
    out[5] = CORE(0x2D8);
}




/* Raw packet engine: the core's test-mode control structure (format 0x1C TX), as libphy's own BLE RF test
 * programs it (bench/idf_phyble/tx_cs_dump.txt). No advertising / connection protocol: the core sends
 * preamble + our sync word + our bytes + CRC (init crcinit) back to back from two descriptors that point
 * at each other, in one event that does not end (MAXEVTIME 0xDEAD). Re-kicked if it ever ends. */
#define RAW_TXD0 0x1400u
#define RAW_TXD1 0x1420u
static uint64_t raw_at;                         /* raw send: absolute start time (half-us), 0 = as soon as possible */
static void raw_kick(void)
{
    if (raw_at) {                                /* scheduled: start time T = CLKN * 625 + (624 - fine) */
        uint64_t T = raw_at; raw_at = 0;
        EM16(0) = 0x7802;
        EM16(2) = (uint16_t)(T / 625); EM16(4) = (uint16_t)((T / 625) >> 16) & 0xFFF; EM16(6) = (uint16_t)(624u - (uint32_t)(T % 625));
        EM16(8) = raw_cs / 2; EM16(10) = 0x03E8; EM16(12) = 0; EM16(14) = 0x0F00;
        CORE(0x18) = 0xFFFFFFFFu;
        CORE(0x100) = 0x80000000u;
        return;
    }
    uint32_t lead = raw_on == 2 ? BLE_FINE : ble_hw_raw_lead;   /* send: 250 us (as arm()); receive: shorter, to re-arm between packets */
    if (raw_on != 2 && ble_hw_raw_jitter) lead += ccount() % ble_hw_raw_jitter;   /* bench: break phase lock with the sender */
    uint32_t t = clkn(), cnt = CORE(0x20) & 0x3FF, f;
    if (cnt > lead) f = cnt - lead; else { f = cnt + 0x270u - lead; t++; }
    EM16(0) = 0x7802;                                     /* libphy's dump read 0x7812 after the run: status [5:3] must start at 0 */
    EM16(2) = t & 0xFFFF; EM16(4) = (t >> 16) & 0xFFF; EM16(6) = (uint16_t)f;
    EM16(8) = raw_cs / 2; EM16(10) = 0x03E8; EM16(12) = 0; EM16(14) = 0x0F00;
    CORE(0x18) = 0xFFFFFFFFu;
    CORE(0x100) = 0x80000000u;
}
int ble_hw_raw_tx(uint8_t ch_idx, uint8_t rate, uint32_t sync, uint32_t crcinit, uint8_t type, const uint8_t *d, unsigned len, uint8_t txpwr)
{
    if (len > 255 || rate > 3 || ch_idx > 39) return -1;
    uint32_t ps = irq_lock();
    scan_chan = 0; adv_on = 0; conns_off();
    static const uint16_t cs[45] = { 0x071C, 0x0080, 0x1100, 0xBEEF, 0xDEAD, 0xBEEF, 0, 0, 0x5555, 0x0055, 0, 0x0A25, 0x0008, 0x001E,
                                     RAW_TXD0, 0x03E8, 0, 0, 0, 0x00E0, 0x00FF, 0, 0xBEEF, 0xDEAD, 0xBEEF };
    for (int k = 0; k < 45; k++) EM16(CS0 + 2 * k) = ble_hw_rx_fmt >= 0x20 ? scan_cs[k] : ble_hw_rx_fmt ? ble_conn_cs[k] : cs[k];
    if (ble_hw_rx_fmt >= 0x20) {                 /* bench: scan template with format (fmt - 0x20) */
        EM16(CS0) = (uint16_t)((EM16(CS0) & ~0x1Fu) | (ble_hw_rx_fmt - 0x20)); EM16(CS0 + 0x20) = 2; EM16(0x14C) = EM16(CS0);
    }
    else if (ble_hw_rx_fmt) {                         /* bench: a connection format for RX, TX blocked (CS+0x18 bits 15:13), long window */
        EM16(CS0) = (uint16_t)((EM16(CS0) & ~0x1Fu) | ble_hw_rx_fmt);
        EM16(CS0 + 0x18) = ble_hw_rx_t18; EM16(CS0 + 0x1A) = 0xFFFF; EM16(CS0 + 0x1C) = 0; EM16(CS0 + 0x20) = 1; EM16(CS0 + 0x28) = 0x00FF;   /* one packet per event: a slave event expects the next one at reply timing (2/event: 2/3 received, more: 1/2) */
    }
    EM16(CS0 + 4) = (uint16_t)((EM16(CS0 + 4) & ~0xFu) | rate | rate << 2);
    EM16(CS0 + 0x0C) = sync & 0xFFFF; EM16(CS0 + 0x0E) = sync >> 16;
    EM16(CS0 + 0x10) = crcinit & 0xFFFF; EM16(CS0 + 0x12) = (crcinit >> 16) & 0xFF;
    EM16(CS0 + 0x16) = ble_hw_rx_fmt ? ch_idx : 0x0A00 | ch_idx;
    EM16(CS0 + 0x18) = txpwr;
    if (d) memcpy(tx_buf, d, len);
    uint16_t w = (uint16_t)((uint16_t)len << 8 | type);
    EM16(RAW_TXD0) = RAW_TXD1; EM16(RAW_TXD0 + 2) = w; EM16(RAW_TXD0 + 4) = TXBUF; EM16(RAW_TXD0 + 6) = 0;
    EM16(RAW_TXD1) = RAW_TXD0; EM16(RAW_TXD1 + 2) = w; EM16(RAW_TXD1 + 4) = TXBUF; EM16(RAW_TXD1 + 6) = 0;
    CORE(0x370) = sync;
    CORE(0x50) = 0x80; CORE(0x54) = 0xA0;
    CORE(0xD0) = ((CORE(0xD0) | 1u << 11) & ~(1u << 12 | 1u << 13 | 1u << 15)) | ble_hw_raw_d0;
    CORE(0x00) = (CORE(0x00) | 1u << 8) & ~(1u << 26);
    if (ble_hw_raw_poke_off & 0x80000000u) EM16(ble_hw_raw_poke_off & 0xFFFF) ^= (uint16_t)ble_hw_raw_poke_or;   /* bench: flip EM bits */
    else if (ble_hw_raw_poke_off) CORE(ble_hw_raw_poke_off) |= ble_hw_raw_poke_or;
    raw_on = 1; raw_cs = CS0;
    raw_kick();
    irq_unlock(ps);
    return 0;
}
/* Raw send from our buffer: the connection-master format (0x02) with our sync word, CRC init, channel, rate and
 * header byte; one packet per event, re-kicked at every event end. No connection exists: the core sends, listens
 * briefly for an answer that never comes, and the event ends. */
int ble_hw_raw_send(uint8_t ch_idx, uint8_t rate, uint32_t sync, uint32_t crcinit, uint8_t hdr0, const uint8_t *d, unsigned len)
{
    return ble_hw_raw_sendn(ch_idx, rate, sync, crcinit, hdr0, d, len, 0);
}
uint64_t ble_hw_now_hu(void) { return now_hu(); }
uint16_t ble_hw_em16(uint32_t off) { return EM16(off); }   /* bench: exchange-memory read */
/* one packet starting at core time t_hu (half-us, from ble_hw_now_hu); at least ~300 us ahead */
int ble_hw_raw_send_at(uint8_t ch_idx, uint8_t rate, uint32_t sync, uint32_t crcinit, uint8_t hdr0, const uint8_t *d, unsigned len, uint64_t t_hu)
{
    if (t_hu < now_hu() + 600u) return -2;
    raw_at = t_hu;
    return ble_hw_raw_sendn(ch_idx, rate, sync, crcinit, hdr0, d, len, 1);
}
int ble_hw_raw_sendn(uint8_t ch_idx, uint8_t rate, uint32_t sync, uint32_t crcinit, uint8_t hdr0, const uint8_t *d, unsigned len, uint32_t count)
{
    if (len > 255 || rate > 3 || ch_idx > 39) return -1;
    uint32_t ps = irq_lock();
    raw_left = count;
    scan_chan = 0; adv_on = 0; conns_off();
    for (int k = 0; k < 45; k++) EM16(CS1 + 2 * k) = (ble_hw_raw_tmpl ? ble_adv_cs : ble_conn_cs)[k];
    EM16(CS1) = (uint16_t)((EM16(CS1) & ~0x1Fu) | ble_hw_raw_fmt);
    EM16(CS1 + 0x18) = ble_hw_raw_t18 != 0xFFFF ? (uint16_t)(ble_hw_raw_t18 & 0x0FFF) : 0x000B;   /* TX power index; the template's bits 15:13 block TX (measured) */
    EM16(CS1 + 4) = (uint16_t)((EM16(CS1 + 4) & ~0xFu) | rate | rate << 2);
    EM16(CS1 + 0x0C) = sync & 0xFFFF; EM16(CS1 + 0x0E) = sync >> 16;
    EM16(CS1 + 0x10) = crcinit & 0xFFFF; EM16(CS1 + 0x12) = (crcinit >> 16) & 0xFF;
    EM16(CS1 + 0x16) = ch_idx;
    EM16(CS1 + 0x20) = RING_N; EM16(CS1 + 0x50) = 0;
    memcpy((uint8_t *)tx_buf + 0x40, d, len);
    EM16(TXC) = 0; EM16(TXC + 2) = (uint16_t)(len << 8 | hdr0); EM16(TXC + 4) = TXCBUF;
    EM16(CS1 + 0x1C) = TXC;
    if (ble_hw_tx_flip_off) EM16(CS1 + ble_hw_tx_flip_off) ^= ble_hw_tx_flip_bits;   /* bench */
    EM16(0x14C) = EM16(CS1);                     /* the current CS format word, as the vendor keeps it */
    for (unsigned k = 0; k < sizeof ble_adv_core_regs / sizeof ble_adv_core_regs[0]; k++)
        if (ble_adv_core_regs[k].off >= 0x2C0) CORE(ble_adv_core_regs[k].off) = ble_adv_core_regs[k].val;
    CORE(0x24) = RING0;
    raw_flags(1);                                /* advertising filter off (it caps packets at 37 bytes), header byte kept, CRC flag */
    raw_on = 2; raw_cs = CS1;
    raw_kick();
    irq_unlock(ps);
    return 0;
}
/* Raw receive: test-mode format 0x0A (libphy's RX test, bench/idf_phyble/rx_cs_dump.txt), our RX ring. */
int ble_hw_raw_rx(uint8_t ch_idx, uint8_t rate, uint32_t sync, uint32_t crcinit)
{
    if (rate > 3 || ch_idx > 39) return -1;
    uint32_t ps = irq_lock();
    scan_chan = 0; adv_on = 0; conns_off();
    static const uint16_t cs[45] = { 0x010A, 0x00C0, 0x1100, 0xBEEF, 0xDEAD, 0xBEEF, 0, 0, 0x5555, 0x0055, 0, 0x0A00, 0x0004, 0xFFFF,
                                     0, 0, RING_N, 0, 0, 0x00E0, 0x00FF, 0, 0xBEEF, 0xDEAD, 0xBEEF };
    for (int k = 0; k < 45; k++) EM16(CS0 + 2 * k) = ble_hw_rx_fmt >= 0x20 ? scan_cs[k] : ble_hw_rx_fmt ? ble_conn_cs[k] : cs[k];
    if (ble_hw_rx_fmt >= 0x20) {                 /* bench: scan template with format (fmt - 0x20) */
        EM16(CS0) = (uint16_t)((EM16(CS0) & ~0x1Fu) | (ble_hw_rx_fmt - 0x20)); EM16(CS0 + 0x20) = 2; EM16(0x14C) = EM16(CS0);
    }
    else if (ble_hw_rx_fmt) {                         /* bench: a connection format for RX, TX blocked (CS+0x18 bits 15:13), long window */
        EM16(CS0) = (uint16_t)((EM16(CS0) & ~0x1Fu) | ble_hw_rx_fmt);
        EM16(CS0 + 0x18) = ble_hw_rx_t18; EM16(CS0 + 0x1A) = 0xFFFF; EM16(CS0 + 0x1C) = 0; EM16(CS0 + 0x20) = 1; EM16(CS0 + 0x28) = 0x00FF;   /* one packet per event: a slave event expects the next one at reply timing (2/event: 2/3 received, more: 1/2) */
    }
    EM16(CS0 + 4) = (uint16_t)((EM16(CS0 + 4) & ~0xFu) | rate | rate << 2);
    EM16(CS0 + 0x0C) = sync & 0xFFFF; EM16(CS0 + 0x0E) = sync >> 16;
    EM16(CS0 + 0x10) = crcinit & 0xFFFF; EM16(CS0 + 0x12) = (crcinit >> 16) & 0xFF;
    EM16(CS0 + 0x16) = ble_hw_rx_fmt ? ch_idx : 0x0A00 | ch_idx;
    for (uint32_t d = RING0; d < RING0 + RING_N * 0x14; d += 0x14) { EM16(d) &= 0x7FFF; EM16(d + 4) = 0; }
    if (ble_hw_rx_big) {                         /* two descriptors, 0x120-byte buffers on our own page (as libphy: 2 descriptors) */
        EM16(RING0) = RING0 + 0x14; EM16(RING0 + 0x12) = TXBUF + 0x200;
        EM16(RING0 + 0x14) = RING0; EM16(RING0 + 0x14 + 0x12) = TXBUF + 0x300;
        raw_rx_n = 2;
    } else raw_rx_n = RING_N;
    if (ble_hw_rx_set_off >= 0x1000) CORE(ble_hw_rx_set_off - 0x1000) = ble_hw_rx_set_val32;   /* bench: core register */
    else if (ble_hw_rx_set_off) EM16(CS0 + ble_hw_rx_set_off) = ble_hw_rx_set_val;               /* bench: CS field */
    if (ble_hw_rx_libphy) { CORE(0x080) = 0x0A640064; CORE(0x090) = 0x00020202; CORE(0x094) = 0x00020202; CORE(0x098) = 0x0F320202; CORE(0x09C) = 0x0F320002; CORE(0x0E0) = 0x019000FA; }
    CORE(0x370) = sync;
    CORE(0xD0) |= 1u << 27;
    CORE(0x24) = RING0;
    CORE(0x00) = (CORE(0x00) | 1u << 8) & ~(1u << 26);
    raw_flags(0);
    raw_on = 1;
    raw_kick();
    irq_unlock(ps);
    return 0;
}
/* next received packet from the ring: 1 and its bytes / RX status / RSSI, or 0 */
int ble_hw_raw_read(uint8_t *buf, unsigned max, unsigned *len, uint16_t *status, int8_t *rssi)
{
    return ble_hw_raw_read2(buf, max, len, status, rssi, 0);
}
int ble_hw_raw_read2(uint8_t *buf, unsigned max, unsigned *len, uint16_t *status, int8_t *rssi, uint8_t *hdr)
{
    return ble_hw_raw_read3(buf, max, len, status, rssi, hdr, 0);
}
int ble_hw_raw_read3(uint8_t *buf, unsigned max, unsigned *len, uint16_t *status, int8_t *rssi, uint8_t *hdr, uint64_t *t_hu)
{
    static uint32_t d = RING0;
    if (d >= RING0 + raw_rx_n * 0x14) d = RING0;
    for (unsigned k = 0; k < raw_rx_n; k++) {
        if (EM16(d) & 0x8000) {
            uint16_t h = EM16(d + 4);
            *status = EM16(d + 2); *rssi = (int8_t)(EM16(d + 6) & 0xFF); *len = h >> 8; if (hdr) *hdr = (uint8_t)h;
            if (t_hu) *t_hu = (uint64_t)(EM16(d + 8) | (uint32_t)EM16(d + 10) << 16) * 625u + (624u - (EM16(d + 0x0C) & 0x3FF));   /* sync time */
            unsigned n = *len < max ? *len : max;
            uint16_t pp = EM16(d + 0x12);
            bool in_tx = pp >= TXBUF && pp + n <= TXBUF + 0x400u;
            if (in_tx) memcpy(buf, (const void *)((uint8_t *)tx_buf + (pp - TXBUF)), n);
            else if (em_span_ok(pp, n)) memcpy(buf, (const void *)em_addr(pp), n);
            else n = 0;                                    /* descriptor points outside our buffers: drop */
            buf[0] = buf[0]; if (n == 0 && max) buf[0] = (uint8_t)h;
            EM16(d) &= 0x7FFF; EM16(d + 4) = 0;
            d = d + 0x14 < RING0 + raw_rx_n * 0x14 ? d + 0x14 : RING0;
            return 1;
        }
        d = d + 0x14 < RING0 + raw_rx_n * 0x14 ? d + 0x14 : RING0;
    }
    return 0;
}
uint32_t ble_hw_raw_rxcount(void) { return CORE(0x374); }
/* raw receive watchdog: an event started in silence can wait without end (coded rates, measured); call often. If no
 * event ended for max_ms, abort it and kick a fresh one. Returns 1 if it re-armed. */
int ble_hw_raw_watchdog(uint32_t max_ms)
{
    if (raw_on != 1) return 0;
    if (ccount() - last_end < max_ms * 240000u) return 0;
    uint32_t ps = irq_lock();
    CORE(0x00) |= 1u << 26;                      /* abort the running event */
    for (volatile int k = 0; k < 2000; k++) {}
    CORE(0x00) &= ~(1u << 26);
    st.aborts++;
    last_end = ccount();
    raw_kick();
    irq_unlock(ps);
    return 1;
}
void ble_hw_raw_stop(void) { raw_on = 0; CORE(0x00) |= 1u << 26; }
int ble_hw_raw_busy(void) { return raw_on != 0; }
uint32_t ble_hw_raw_txcount(void) { return CORE(0xD4); }
uint16_t ble_hw_em_rd(uint32_t off) { return EM16(off); }
void ble_hw_em_wr(uint32_t off, uint16_t v) { EM16(off) = v; }
