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

static uint32_t em_buf[BLE_EM_WORDS];
static volatile uint8_t scan_chan;            /* 0: stopped */
static volatile uint32_t last_end;            /* ccount of the last event end (watchdog) */
static ble_hw_stats_t st;
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
        uint32_t n = (q_head + 1) % Q_N;
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
    EM16(0x42E) = 0x40;                          /* MAXEVTIME: bounded event */
    uint32_t t = clkn() + 1;                     /* next half-slot (measured: 1 and 3 work, 2 is never picked up) */
    EM16(0) = EM16(0) & ~0x38u;
    EM16(2) = t & 0xFFFF; EM16(4) = (t >> 16) & 0xFFF; EM16(6) = 0;
    CORE(0x24) = RING0;
    CORE(0x18) = 0xFFFFFFFFu;                    /* stale status blocks the next start (measured) */
    CORE(0x100) = 0x80000000u;
}

/* Event end/skip handling runs in thread context (ble_hw_service), not in the interrupt: re-arming from the
 * interrupt handler wedges the scheduler within a few events (measured, many variants; root cause open).
 * The interrupt only masks the core's interrupt and wakes the CPU. */
static void isr(void *arg) { (void)arg; st.isr_calls++; CORE(0x0C) = 0; }

static void service(void)
{
    uint32_t raw = CORE(0x14);
    if (raw & (INT_END | INT_SKIP)) {
        uint32_t c0 = ccount();
        CORE(0x18) = raw;
        for (int k = 0; k < 32; k++) {           /* ESP interrupt FIFO 0x2D8: [9:5] pending, write bit 0 to pop (as ROM r_rwble_isr) */
            uint32_t f = CORE(0x2D8);
            if (!((f >> 5) & 0x1F)) break;
            CORE(0x2D8) = f | 1;
        }
        st.events++;
        last_end = c0;
        if (scan_chan) arm(); else drain();
        st.isr_cycles += ccount() - c0;
    }
    CORE(0x0C) = INT_END | INT_SKIP;
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
    for (int k = 0; k < 64; k++) R32(0x60011000u + 4 * k) = ble_modem_regs[k];
    memcpy(em_buf, ble_em_image, sizeof em_buf);
    for (unsigned k = 0; k < sizeof ble_em_segs / sizeof ble_em_segs[0]; k++) {
        unsigned i = ble_em_segs[k].idx;
        uint32_t a = (uint32_t)em_buf + ble_em_segs[k].pos;
        R32(0x60031204u + 4 * i + (i > 47 ? 28 : 0)) = ((uint32_t)ble_em_segs[k].em_off >> 2) << 18 | ((a >> 2) & 0x3FFFF);
    }
    for (unsigned k = 0; k < sizeof ble_core_regs / sizeof ble_core_regs[0]; k++) CORE(ble_core_regs[k].off) = ble_core_regs[k].val;
    CORE(0x0C) = 0;
    CORE(0x00) = ble_rwblecntl;
    for (uint32_t d = RING0; d < RING0 + RING_N * 0x14; d += 0x14) { EM16(d) &= 0x7FFF; EM16(d + 4) = 0; }
    CORE(0x18) = 0xFFFFFFFFu;
CORE(0x0C) = INT_END | INT_SKIP;
    return irq_attach(SRC_RWBLE, 1, isr, 0) < 0 ? -1 : 0;
}

void ble_hw_scan(uint8_t chan)
{
    scan_chan = chan;
    last_end = ccount();
    arm();
}

void ble_hw_stop(void) { scan_chan = 0; }

int ble_hw_read(ble_hw_pkt_t *p)
{
    service();
    if (scan_chan && ccount() - last_end > 240000u * 50) {   /* no event end for 50 ms: re-arm (a lost start) */
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

int ble_hw_wait(ble_hw_pkt_t *p, uint32_t timeout_us)
{
    uint32_t t0 = ccount();
    for (;;) {
        if (ble_hw_read(p)) return 1;
        if (ccount() - t0 >= timeout_us * 240u) return 0;
        __asm__ volatile("waiti 0");             /* sleep until an interrupt (the core's, or the tick) */
    }
}

void ble_hw_debug(uint32_t out[6])
{
    out[0] = CORE(0x0C); out[1] = CORE(0x14); out[2] = EM16(0); out[3] = clkn(); out[4] = CORE(0x24);
    out[5] = CORE(0x2D8);
}

