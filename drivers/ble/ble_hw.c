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

#define R32(a)  (*(volatile uint32_t *)(a))
#define CORE(o) R32(0x60031000u + (o))
#define SRC_RWBLE 8
#define RING0   0x1000u
#define RING_N  10u
#define INT_END (1u << 5)
#define INT_SKIP (1u << 8)
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
static volatile uint8_t scan_chan;            /* 0: stopped */
static volatile uint32_t last_end;            /* ccount of the last event end (watchdog) */
static volatile uint8_t adv_on;              /* advertising instead of scanning */
static uint32_t adv_next, adv_iv;             /* next advert start / interval, half-slots */
static uint16_t scan_cs[45];                  /* scan control structure, restored by ble_hw_scan */
static uint32_t tx_buf[64];                   /* EM 0x2400 (page register 7): +0 advertising payload, +0x40.. data TX */
#define CS0     0x400u
#define TXD     0x1400u
#define TXBUF   0x2400u
static ble_hw_stats_t st;

/* ---- connection (slave) ---------------------------------------------------------------------- */
#define CS1     0x45Au                           /* slave connection control structure (CS index 1) */
#define TXC     0x1440u                          /* our data TX descriptor */
#define TXCBUF  (TXBUF + 0x40u)
static volatile uint8_t conn_on;
static uint8_t own_addr[6];
static struct {
    uint32_t aa, crcinit;
    uint16_t interval, latency, timeout, evt, instant_upd, instant_map;
    uint8_t  csa2, hop, last_unmapped, chm[5], nused, used[37], upd_pending, map_pending, new_chm[5];
    uint16_t new_interval, new_latency, new_timeout, new_winoff; uint8_t new_winsize;
    uint64_t anchor;                             /* half-us: start of the window of the next event */
    uint32_t win;                                /* half-us: window length */
    uint32_t last_rx_ms;                         /* ms clock of the last PDU from the master */
    uint32_t idx;                                /* exchange-table entry rotation */
} cn;
static volatile uint8_t tx_busy;
#define TXQ_N 8u
static struct { uint8_t llid, len, d[27]; } txq[TXQ_N];
static volatile uint32_t txq_head, txq_tail;
static uint8_t init_addr[6];
static uint32_t ms_now(void) { return (uint32_t)(time_us() / 1000); }
#define Q_N 32u
static ble_hw_pkt_t q[Q_N];
static volatile uint32_t q_head, q_tail;

static uint32_t em_addr(uint32_t off)
{
    unsigned b = 0;
    for (unsigned k = 0; k < sizeof ble_em_segs / sizeof ble_em_segs[0]; k++)
        if (ble_em_segs[k].em_off <= off && ble_em_segs[k].em_off >= ble_em_segs[b].em_off) b = k;
    return (uint32_t)em_buf + ble_em_segs[b].pos + off - ble_em_segs[b].em_off;
}
#define EM16(off) (*(volatile uint16_t *)em_addr(off))

static uint32_t clkn(void)
{
    CORE(0x1C) = (CORE(0x1C) & 0x7FFFFFFFu) | 0x80000000u;
    while (CORE(0x1C) & 0x80000000u) {}
    return CORE(0x1C) & 0x0FFFFFFFu;
}

static void drain(void)
{
    for (uint32_t d = RING0; d < RING0 + RING_N * 0x14; d += 0x14) {
        if (!(EM16(d) & 0x8000)) continue;
        if (EM16(d + 2) & 0x000F) {               /* RX status [3:0]: sync, RX time, length, CRC error (bit 6 etc. are normal flags) */
            st.rx_errors++; EM16(d) &= 0x7FFF; EM16(d + 4) = 0; continue;
        }
        uint32_t n = (q_head + 1) % Q_N;
        if (conn_on && cn.evt == 0 && !memcmp((const void *)em_addr(EM16(d + 0x12)), init_addr, 6)) { EM16(d) &= 0x7FFF; EM16(d + 4) = 0; continue; }
        if (n == q_tail) st.dropped++;
        else {
            ble_hw_pkt_t *p = &q[q_head];
            uint16_t ph = EM16(d + 4), ch = EM16(d + 6);
            p->hdr0 = ph & 0xFF; p->len = ph >> 8; p->rssi = (int8_t)(ch & 0xFF); p->chan = (ch >> 8) & 0x3F;
            p->clkn = EM16(d + 8) | (uint32_t)EM16(d + 10) << 16;
            uint32_t len = p->len > 37 ? 37 : p->len;
            memcpy(p->data, (const void *)em_addr(EM16(d + 0x12)), len);
            q_head = n;
        }
        st.packets++;
        EM16(d) &= 0x7FFF; EM16(d + 4) = 0;
    }
}

/* one scan event (one receive window) starting at the next half-slot. Drains first: the core can set a
 * descriptor's done bit after the end-of-event interrupt, and an event kicked onto a done descriptor
 * never receives and never ends (measured). */
static void arm(void)
{
    drain();
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
static void service(void);
static void isr(void *arg)
{
    (void)arg; st.isr_calls++; CORE(0x0C) = 0;
    service();
}

static uint8_t adv_csa2 = 1;                  /* advertise CSA#2 support (ChSel); ble_hw_csa2(0) forces CSA#1 */
void ble_hw_csa2(int on) { adv_csa2 = (uint8_t)on; }
static uint8_t adv_type = 2;                  /* 0 ADV_IND (connectable), 2 ADV_NONCONN_IND */
void ble_hw_adv_type(uint8_t t) { adv_type = t; }
static uint32_t adv_idx;                      /* the vendor rotates exchange-table entries 0..15, one per advert */
static void arm_adv(void)
{
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

static void conn_arm(void)                       /* the window of event cn.evt, on its channel */
{
    EM16(CS1 + 0x16) = cn.csa2 ? csa2_ch(cn.evt) : csa1_next();              /* fixed channel this event (hardware hopping off) */
    uint32_t w = cn.win / 2;                     /* RXWINCNTL in us (RXWIDE 0) */
    EM16(CS1 + 0x1A) = (uint16_t)(w > 0x7FFF ? 0x7FFF : w);
    uint64_t st_hu = cn.anchor;
    uint32_t t = (uint32_t)(st_hu / 625u), f = 624u - (uint32_t)(st_hu % 625u);
    cn.idx = (cn.idx + 1) & 15;
    uint32_t e = 16 * cn.idx;
    EM16(e + 0) = ble_conn_et[0] & ~0x38u;
    EM16(e + 2) = t & 0xFFFF; EM16(e + 4) = (t >> 16) & 0xFFF; EM16(e + 6) = f;
    EM16(e + 8) = CS1 / 2; EM16(e + 10) = ble_conn_et[5]; EM16(e + 12) = ble_conn_et[6]; EM16(e + 14) = ble_conn_et[7];
    CORE(0x24) = RING0;
    CORE(0x18) = 0xFFFFFFFFu;
    CORE(0x100) = 0x80000000u | cn.idx;
}

static void conn_start(const uint8_t *p, uint64_t adv_start_hu, uint8_t hdr0)   /* p: CONNECT_IND payload (34 bytes) */
{
    cn.csa2 = (hdr0 >> 5) & 1;                   /* ChSel in the CONNECT_IND header: both sides support CSA#2 */
    const uint8_t *ll = p + 12;
    cn.aa = ll[0] | ll[1] << 8 | ll[2] << 16 | (uint32_t)ll[3] << 24;
    cn.crcinit = ll[4] | ll[5] << 8 | ll[6] << 16;
    uint8_t winsize = ll[7]; uint16_t winoff = ll[8] | ll[9] << 8;
    cn.interval = ll[10] | ll[11] << 8; cn.latency = ll[12] | ll[13] << 8; cn.timeout = ll[14] | ll[15] << 8;
    cn.hop = ll[21] & 0x1F; cn.last_unmapped = 0; cn.evt = 0; cn.upd_pending = cn.map_pending = 0;
    for (int k = 0; k < 45; k++) EM16(CS1 + 2 * k) = ble_conn_cs[k];
    EM16(CS1 + 0x0C) = cn.aa & 0xFFFF; EM16(CS1 + 0x0E) = cn.aa >> 16;
    EM16(CS1 + 0x10) = cn.crcinit & 0xFFFF; EM16(CS1 + 0x12) = cn.crcinit >> 16;
    EM16(CS1 + 0x1C) = 0;                        /* no TX descriptor: the core answers with empty PDUs */
    EM16(CS1 + 0x20) = RING_N;
    EM16(CS1 + 0x50) = 0;
    chm_apply(ll + 16);
    /* CONNECT_IND end: advert start + radio ramp (~100 us) + ADV_IND air + 150 us + CONNECT_IND air (352 us) */
    uint32_t adv_air = 8u * (1 + 4 + 2 + (EM16(TXD + 2) >> 8) + 3);
    uint64_t ci_end = adv_start_hu + 2u * (100 + adv_air + 150 + 352);
    uint64_t tw = ci_end + 2u * 1250u * (1u + winoff);          /* transmit window start */
    cn.anchor = tw - 2u * 200u;
    cn.win = 2u * (1250u * winsize + 400u);
    cn.last_rx_ms = ms_now();
    q_tail = q_head;                             /* advertising-phase PDUs (incl. the CONNECT_IND) are not link data */
    memcpy(init_addr, p, 6);
    txq_head = txq_tail = 0;
    adv_on = 0; conn_on = 1; tx_busy = 0;
    conn_arm();
}

static void tx_load(void)                        /* next queued PDU into the descriptor once the core is done with the last */
{
    if (tx_busy && !(EM16(TXC) & 0x8000) && EM16(CS1 + 0x1C)) return;   /* still in flight */
    tx_busy = 0;
    if (txq_tail == txq_head) return;
    memcpy((uint8_t *)tx_buf + 0x40, txq[txq_tail].d, txq[txq_tail].len);
    EM16(TXC) = 0;
    EM16(TXC + 2) = (uint16_t)(txq[txq_tail].len << 8 | txq[txq_tail].llid);
    EM16(TXC + 4) = TXCBUF;
    EM16(CS1 + 0x1C) = TXC;
    txq_tail = (txq_tail + 1) % TXQ_N;
    tx_busy = 1;
    st.tx_pdus++;
}

static void arm_adv(void);
static void conn_end(void)                       /* link lost or terminated: back to advertising */
{
    conn_on = 0;
    st.disconnects++;
    for (int e = 0; e < 16; e++) EM16(16 * e + 0) = 0x281A;
    adv_on = 1;
    adv_next = clkn() + 16;
    arm_adv();
}

/* after each connection event: next anchor, pending updates at their instant, supervision */
/* anchor tracking: after an event with a PDU, the next window opens SYNC_LEAD before the expected sync
 * (last sync + interval) and lasts 2 * SYNC_LEAD; without a PDU it widens by WIDEN per missed event. */
#define SYNC_LEAD (2u * 300u)                    /* half-us */
#define WIDEN     (2u * 50u)
static uint64_t last_sync;
static uint32_t missed;
static void conn_next(int got, uint64_t sync)
{
    tx_load();
    if (got) cn.last_rx_ms = ms_now();
    if (ms_now() - cn.last_rx_ms > cn.timeout * 10u) { conn_end(); return; }
    cn.evt++;
    EM16(CS1 + 0x50) = cn.evt;
    uint64_t iv = 2u * 1250u * cn.interval;
    if (cn.upd_pending && cn.evt == cn.instant_upd) {           /* connection update: new window at the instant */
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
    uint64_t now = now_hu();
    while ((int64_t)(cn.anchor - now) < 2 * 1000) {        /* never schedule into the past: skip whole events */
        cn.anchor += 2u * 1250u * cn.interval; cn.evt++; if (!cn.csa2) csa1_next();
    }
    conn_arm();
}

static void service(void)
{
    uint32_t raw = CORE(0x14);
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
        st.events++;
        if (raw & INT_SKIP) st.skips++;
        last_end = c0;
        if (conn_on) {
            uint32_t before = st.packets;
            uint64_t sync = 0;                     /* first PDU of the event: CLKN + fine count at sync (RXCLKNSYNC, RXFCNTSYNC[9:0]) */
            uint32_t best = 0xFFFFFFFF;
            for (uint32_t d = RING0; d < RING0 + RING_N * 0x14; d += 0x14) if ((EM16(d) & 0x8000) && !(EM16(d + 2) & 0x000F)) {
                uint32_t ck = EM16(d + 8) | (uint32_t)EM16(d + 10) << 16;
                if (ck < best) { best = ck; sync = (uint64_t)ck * 625u + (624u - (EM16(d + 0x0C) & 0x3FF)); }
            }
            drain(); conn_next(st.packets != before, sync);
        }
        else if (adv_on) {
            uint64_t adv_start = (uint64_t)adv_next * 625u;          /* this advert started at adv_next (fine 0x270 = slot start) */
            uint32_t h0 = q_head; drain();
            int connected = 0;
            for (uint32_t k = h0; k != q_head; k = (k + 1) % Q_N)   /* CONNECT_IND to our address? */
                if ((q[k].hdr0 & 0xF) == 5 && q[k].len == 34 && !memcmp(q[k].data + 6, own_addr, 6)) { conn_start(q[k].data, adv_start, q[k].hdr0); connected = 1; break; }
            if (!connected) { adv_next += adv_iv; arm_adv(); }
        }
        else if (scan_chan) arm(); else drain();
        st.isr_cycles += ccount() - c0;
    }
    CORE(0x0C) = BLE_MASK;
}

int ble_hw_init(void)
{
    rf_freq_mem_load();                          /* per-MHz LO table the core tunes from */
    R32(0x60026014) = 0xFFFFDFDFu;               /* modem clocks incl. BT (SYSCON) */
    R32(0x60026018) = 0;
    for (unsigned k = 0; k < sizeof ble_vendor_phy / sizeof ble_vendor_phy[0]; k++) {
        uint32_t a = ble_vendor_phy[k].addr;     /* coex arbiter: grant the radio to BT (page 0; 3 mirrors) */
        if (a >= 0x60035000u && a < 0x60035400u && a != 0x60035000u && a != 0x6003507Cu) R32(a) = ble_vendor_phy[k].val;
    }
    /* BT baseband / analog-bus sequencer 0x6000E000: loaded while E0C4 bit 25 is held (libbtbb does the same) */
    R32(0x6000E0C4) |= 1u << 25;
    uint32_t c4 = 0;
    for (unsigned k = 0; k < sizeof ble_vendor_phy / sizeof ble_vendor_phy[0]; k++) {
        uint32_t a = ble_vendor_phy[k].addr;
        if (a == 0x6000E0C4u) c4 = ble_vendor_phy[k].val;
        else if (a >= 0x6000E000u && a < 0x6000E200u) R32(a) = ble_vendor_phy[k].val;
    }
    R32(0x600060FC) = (R32(0x600060FC) & 0xFFFF00FFu) | 0xC800u;
    R32(0x6000E0C4) = c4 & ~(1u << 25);
    for (int k = 0; k < 64; k++) {               /* 0x6C..0x88, 0xB0 are run-time state (captured mid-scan): leave them */
        if ((k >= 0x6C / 4 && k <= 0x88 / 4) || k == 0xB0 / 4) continue;
        R32(0x60011000u + 4 * k) = ble_modem_regs[k];
    }
    memcpy(em_buf, ble_em_image, sizeof em_buf);
    for (int k = 0; k < 56; k++) R32(0x60031204u + 4 * k + (k > 47 ? 28 : 0)) = ble_em_map_off[k];   /* offset-only entries */
    for (unsigned k = 0; k < sizeof ble_em_segs / sizeof ble_em_segs[0]; k++) {
        unsigned i = ble_em_segs[k].idx;
        uint32_t a = (uint32_t)em_buf + ble_em_segs[k].pos;
        R32(0x60031204u + 4 * i + (i > 47 ? 28 : 0)) = ((uint32_t)ble_em_segs[k].em_off >> 2) << 18 | ((a >> 2) & 0x3FFFF);
    }
    for (unsigned k = 0; k < sizeof ble_core_regs / sizeof ble_core_regs[0]; k++) CORE(ble_core_regs[k].off) = ble_core_regs[k].val;
    R32(0x60031204u + 4 * 7) = (TXBUF >> 2) << 18 | (((uint32_t)tx_buf >> 2) & 0x3FFFF);   /* EM page for the TX payload */
    CORE(0x2C4) |= 1u << 7;                      /* page-valid mask (pages 0..31; 0x2C8: 32..47, 0x300: 48..): the core
                                                  * ignores a mapped page whose bit is clear (vendor: r_emi_alloc_em_mapping) */
    for (int k = 0; k < 45; k++) scan_cs[k] = EM16(CS0 + 2 * k);
    CORE(0x0C) = 0;
    CORE(0x00) = ble_rwblecntl;
    for (uint32_t d = RING0; d < RING0 + RING_N * 0x14; d += 0x14) { EM16(d) &= 0x7FFF; EM16(d + 4) = 0; }
    CORE(0x18) = 0xFFFFFFFFu;
CORE(0x0C) = INT_END | INT_SKIP;
    return irq_attach(SRC_RWBLE, 1, isr, 0) < 0 ? -1 : 0;
}

void ble_hw_scan(uint8_t chan)
{
    adv_on = 0;
    for (int k = 0; k < 45; k++) EM16(CS0 + 2 * k) = scan_cs[k];
    scan_chan = chan;
    last_end = ccount();
    arm();
}

void ble_hw_stop(void) { scan_chan = 0; adv_on = 0; conn_on = 0; }

int ble_hw_connected(void) { return conn_on; }
int ble_hw_conn_info(uint8_t *csa2, uint16_t *interval) { *csa2 = cn.csa2; *interval = cn.interval; return conn_on; }

/* queue one LL data PDU (llid 2: L2CAP start, 3: LL control); -1 while the previous one is not acknowledged */
int ble_hw_send(uint8_t llid, const uint8_t *data, unsigned len)
{
    if (!conn_on || len > 27) return -1;
    uint32_t ps = irq_lock();
    uint32_t n = (txq_head + 1) % TXQ_N;
    if (n == txq_tail) { irq_unlock(ps); return -1; }     /* queue full */
    txq[txq_head].llid = llid; txq[txq_head].len = (uint8_t)len; memcpy(txq[txq_head].d, data, len);
    txq_head = n;
    tx_load();
    irq_unlock(ps);
    return 0;
}

/* LL control the link layer must answer; the rest goes to the application. Returns 1 if consumed. */
int ble_hw_ll_control(const ble_hw_pkt_t *p)
{
    if ((p->hdr0 & 3) != 3 || !p->len) return 0;
    const uint8_t *d = p->data; uint8_t r[27];
    switch (d[0]) {
    case 0x00:                                   /* LL_CONNECTION_UPDATE_IND: WinSize WinOffset Interval Latency Timeout Instant */
        cn.new_winsize = d[1]; cn.new_winoff = d[2] | d[3] << 8; cn.new_interval = d[4] | d[5] << 8;
        cn.new_latency = d[6] | d[7] << 8; cn.new_timeout = d[8] | d[9] << 8; cn.instant_upd = d[10] | d[11] << 8; cn.upd_pending = 1;
        return 1;
    case 0x01:                                   /* LL_CHANNEL_MAP_IND */
        memcpy(cn.new_chm, d + 1, 5); cn.instant_map = d[6] | d[7] << 8; cn.map_pending = 1; return 1;
    case 0x02: { uint32_t ps = irq_lock(); if (conn_on) conn_end(); irq_unlock(ps); return 1; }   /* LL_TERMINATE_IND */
    case 0x08: case 0x0E:                        /* LL_FEATURE_REQ / LL_PERIPHERAL_FEATURE_REQ -> LL_FEATURE_RSP, no features */
        r[0] = 0x09; memset(r + 1, 0, 8); ble_hw_send(3, r, 9); return 1;
    case 0x0C:                                   /* LL_VERSION_IND: 5.0, Espressif (0x02E5) */
        r[0] = 0x0C; r[1] = 9; r[2] = 0xE5; r[3] = 0x02; r[4] = 0; r[5] = 0; ble_hw_send(3, r, 6); return 1;
    case 0x12: r[0] = 0x13; ble_hw_send(3, r, 1); return 1;     /* LL_PING_REQ */
    case 0x14:                                   /* LL_LENGTH_REQ -> 27 bytes, 328 us both ways */
        r[0] = 0x15; r[1] = 27; r[2] = 0; r[3] = 0x48; r[4] = 0x01; r[5] = 27; r[6] = 0; r[7] = 0x48; r[8] = 0x01; ble_hw_send(3, r, 9); return 1;
    case 0x07: case 0x09: case 0x0D: case 0x13: case 0x15: return 1;   /* responses / reject: nothing to do */
    default: r[0] = 0x07; r[1] = d[0]; ble_hw_send(3, r, 2); return 1; /* LL_UNKNOWN_RSP */
    }
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
    service();
    if (scan_chan && !adv_on && ccount() - last_end > 240000u * 50) {   /* no event end for 50 ms: re-arm (a lost start) */
        uint32_t ps = irq_lock();
        CORE(0x00) |= 1u << 24;                  /* SCAN_ABORT: a stuck event refuses new starts */
        for (volatile int k = 0; k < 200; k++) {}
        CORE(0x00) &= ~(1u << 24);
        st.aborts++;
        last_end = ccount(); arm(); irq_unlock(ps);
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
    uint32_t e = 16 * cn.idx;
    out[0] = CORE(0x0C); out[1] = CORE(0x14); out[2] = EM16(e) | (uint32_t)EM16(CS1 + 0x1A) << 16; out[3] = clkn(); out[4] = EM16(e + 2) | (uint32_t)EM16(e + 4) << 16;
    out[5] = CORE(0x2D8);
}



