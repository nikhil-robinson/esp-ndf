/* rf_pkt: the hardware packet engine (rf.h) driven by line commands, for tools/rf_pkt_test.py (two boards).
 *   send mhz rate sync flags len count     payload 00 01 02 ...; count 0 = continuous; prints packets/s over 1 s
 *   rx mhz rate sync flags gain ms         receive for ms: good / CRC-bad packets, wrong bytes, RSSI, CPU
 *   stop | limits min_khz max_khz */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include "rf.h"
#include "psram.h"
#include "ble_hw.h"
#include "../../drivers/rf/rf_init.h"
#include "../../drivers/rf/rf_regi2c.h"
#include "../../drivers/rf/rf_dump.h"
#include "../../drivers/rf/rf_dump.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static char line[96];
static uint8_t payload[255];
static uint8_t txpower;                            /* 0 = default */


/* bench (demodulator item): a packet-engine frame as GFSK from the DAC with any bit rate and deviation. Frame as the
 * engine sends it: preamble (opposite to the sync word's first bit; 2 bytes at >= 1500 kbit/s), sync 0x71764129, header
 * 0x5A, length, payload 0..len-1, CRC-24 (init 0x555555), whitening by the channel index; LSB first; BT 0.5. */
static uint8_t swapbits(uint8_t a) { uint8_t v = 0; for (int k = 0; k < 8; k++) if (a & (1 << k)) v |= 0x80 >> k; return v; }
static int gfsk_frame(uint8_t *f, int chan, int len, int pre2)
{
    int n = 0; const uint32_t sync = 0x71764129u;
    uint8_t pre = (sync & 1) ? 0x55 : 0xAA;
    f[n++] = pre; if (pre2) f[n++] = pre;
    for (int k = 0; k < 4; k++) f[n++] = (uint8_t)(sync >> (8 * k));
    uint8_t *pdu = f + n; pdu[0] = 0x5A; pdu[1] = (uint8_t)len;
    for (int k = 0; k < len; k++) pdu[2 + k] = (uint8_t)k;
    int pl = 2 + len; uint8_t c[3] = { 0x55, 0x55, 0x55 };
    for (int k = 0; k < pl; k++) {
        uint8_t d = pdu[k];
        for (int v = 0; v < 8; v++, d >>= 1) {
            uint8_t t = c[0] >> 7;
            c[0] = (uint8_t)(c[0] << 1 | c[1] >> 7); c[1] = (uint8_t)(c[1] << 1 | c[2] >> 7); c[2] <<= 1;
            if (t != (d & 1)) { c[2] ^= 0x5B; c[1] ^= 0x06; }
        }
    }
    for (int k = 0; k < 3; k++) pdu[pl + k] = swapbits(c[k]);
    uint8_t wc = swapbits((uint8_t)chan) | 2;
    for (int k = 0; k < pl + 3; k++) for (uint8_t m = 1; m; m <<= 1) { if (wc & 0x80) { wc ^= 0x11; pdu[k] ^= m; } wc <<= 1; }
    return n + pl + 3;
}
static int gfsk_play(uint32_t khz, int chan, float rate, float dev, int len, int amp, float gap_us)
{
    static uint8_t f[64]; if (len > 50) return -1;
    int nb = gfsk_frame(f, chan, len, rate >= 1.5e6f) * 8;
    float sps = 40e6f / rate; uint32_t n = (uint32_t)((float)nb * sps + gap_us * 40);
    if (n > RF_TX_MAX_WORDS) return -2;
    static float g[61]; float gs = 0;
    for (int k = 0; k < 61; k++) { float t = ((float)k - 30) / 10 / 0.2650f; g[k] = expf(-0.5f * t * t); gs += g[k]; }   /* 10 taps per bit, +/-3 bits */
    uint32_t *b = (uint32_t *)0x3FCB0000u;          /* dump bank 0: idle while transmitting */
    float ph = 0;
    for (uint32_t k = 0; k < n; k++) {
        float x = 0, tb = (float)k / sps;           /* time in bits */
        if (tb < (float)nb + 3) for (int j = 0; j < 61; j++) {
            int bit = (int)floorf(tb + ((float)j - 30) / 10);
            float v = bit < 0 || bit >= nb ? 0 : ((f[bit >> 3] >> (bit & 7)) & 1 ? 1.f : -1.f);
            x += v * g[j];
        }
        ph += 2 * 3.14159265f * dev * (x / gs) / 40e6f;
        if (ph > 3.14159265f) ph -= 2 * 3.14159265f;
        if (ph < -3.14159265f) ph += 2 * 3.14159265f;
        b[k] = tb < (float)nb + 3 ? rf_iq_word((int)lrintf((float)amp * cosf(ph)), (int)lrintf((float)amp * sinf(ph))) : rf_iq_word(0, 0);
    }
    rf_pkt_stop(); rf_tx_stop();
    int rc = rf_set_freq_khz(khz); if (!rc) rc = rf_tx_start(); if (!rc) rc = rf_tx_play(b, n, 1);
    return rc ? rc : (int)n;
}
static int chan_of_mhz(int mhz)                  /* as rf_pkt.c chan_of */
{
    if (mhz < 2402 || mhz > 2480 || (mhz & 1)) return -1;
    if (mhz == 2402) return 37;
    if (mhz == 2426) return 38;
    if (mhz == 2480) return 39;
    return mhz < 2426 ? (mhz - 2404) / 2 : (mhz - 2406) / 2;
}

static uint32_t poke[8][3]; static unsigned npoke;
static int16_t sbuf[2 * 512];                    /* bench: decimated samples */
static void command(void)
{
    char *v[10]; int n = 0;
    for (char *p = line; *p && n < 10; ) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        v[n++] = p; while (*p && *p != ' ') p++;
    }
    if (!n) return;
    if (!strcmp(v[0], "send") && n > 6) {
        rf_pkt_cfg_t c = { (uint16_t)atoi(v[1]), (uint8_t)atoi(v[2]), (uint8_t)atoi(v[4]), (uint32_t)strtoul(v[3], 0, 0), 0x555555, txpower };
        uint32_t cnt = (uint32_t)strtoul(v[6], 0, 0);
        ble_hw_stats_t s0, s1; ble_hw_get_stats(&s0); uint32_t t0 = ccount();
        int rc = rf_pkt_send(&c, 0x5A, payload, (unsigned)atoi(v[5]), cnt);
        if (rc) { printf("send rc %d\n", rc); return; }
        if (cnt) { while (rf_pkt_busy() && ccount() - t0 < 240000000u * 10) {} ble_hw_get_stats(&s1);
                   printf("send rc 0 sent %lu in %lu ms\n", (unsigned long)(s1.events - s0.events), (unsigned long)((ccount() - t0) / 240000)); return; }
        delay_ms(1000); ble_hw_get_stats(&s1);
        printf("send rc 0 packets/s %lu cpu %.3f%% ctl %08lx\n", (unsigned long)(s1.events - s0.events), (double)(s1.isr_cycles - s0.isr_cycles) / 2.4e6, (unsigned long)*(volatile uint32_t *)0x60031000);
    } else if (!strcmp(v[0], "rx") && n > 6) {
        rf_pkt_cfg_t c = { (uint16_t)atoi(v[1]), (uint8_t)atoi(v[2]), (uint8_t)atoi(v[4]), (uint32_t)strtoul(v[3], 0, 0), 0x555555, 0 };
        int rc = rf_pkt_rx_start(&c, atoi(v[5]));
        if (rc) { printf("rx rc %d\n", rc); return; }
        for (unsigned k = 0; k < npoke; k++) *(volatile uint32_t *)poke[k][0] = (*(volatile uint32_t *)poke[k][0] & ~poke[k][1]) | poke[k][2];   /* bench: rxset */
        extern uint32_t rf_pkt_core_resets; uint32_t cr0 = rf_pkt_core_resets;
        ble_hw_stats_t q0, q1; ble_hw_get_stats(&q0); uint32_t st1 = 0, st1_last = 0;
        static rf_pkt_t p; uint32_t good = 0, crc = 0, wrong = 0, lens = 0, t0 = ccount(), dur = (uint32_t)atoi(v[6]) * 240000u; int32_t rs = 0; uint8_t hdr = 0; uint32_t crcdata = 0, empty = 0; int rmin = 127, rmax = -128; uint64_t tlast = 0, dsum = 0; uint32_t dmin = 0xFFFFFFFF, dmax = 0, nd = 0;
        while (ccount() - t0 < dur) {
            if (!rf_pkt_read(&p)) continue;
            if (p.status & 1) { st1++; st1_last = p.status | (uint32_t)p.len << 16 | (uint32_t)(uint8_t)p.rssi << 24; continue; }   /* window ended without a packet */
            if (!p.len) { empty++; continue; }
            if (!p.ok) { crc++; unsigned m = 0; while (m < p.len && p.data[m] == (uint8_t)m) m++; if (p.len && m == p.len) crcdata++; continue; }
            if (good && p.time_us > tlast) { uint32_t dt = (uint32_t)(p.time_us - tlast); if (dt < dmin) dmin = dt; if (dt > dmax) dmax = dt; dsum += dt; nd++; }
            tlast = p.time_us;
            good++; rs += p.rssi; lens = p.len; hdr = p.hdr; if (p.rssi < rmin) rmin = p.rssi; if (p.rssi > rmax) rmax = p.rssi;
            for (unsigned k = 0; k < p.len; k++) if (p.data[k] != (uint8_t)k) {
                if (!wrong++) { printf("rx bad"); for (unsigned j = 0; j < p.len; j++) printf(" %02x", p.data[j]); printf("\n"); }   /* bench: first wrong packet */
                break; }
        }
        uint32_t ctl = *(volatile uint32_t *)0x60031000; ble_hw_get_stats(&q1);
        printf("rx events %lu isr %lu aborts %lu intstat %08lx intraw %08lx nosync %lu last %08lx resets %lu\n", (unsigned long)(q1.events - q0.events), (unsigned long)(q1.isr_calls - q0.isr_calls), (unsigned long)(q1.aborts - q0.aborts), (unsigned long)*(volatile uint32_t *)0x60031010, (unsigned long)*(volatile uint32_t *)0x60031014, (unsigned long)st1, (unsigned long)st1_last, (unsigned long)(rf_pkt_core_resets - cr0));
        rf_pkt_stop();
        printf("rx ctl %08lx\n", (unsigned long)ctl);
        printf("rx empty %lu rssi_min %d rssi_max %d\n", (unsigned long)empty, rmin, rmax);
        printf("rx intervals n %lu min %lu max %lu mean %.2f us\n", (unsigned long)nd, (unsigned long)dmin, (unsigned long)dmax, nd ? (double)dsum / nd : 0.0);
        printf("rx rc 0 good %lu crcbad %lu crcbad_dataok %lu wrong %lu len %lu hdr %02x rssi %ld\n", (unsigned long)good, (unsigned long)crc, (unsigned long)crcdata, (unsigned long)wrong, (unsigned long)lens, hdr, (long)(good ? rs / (int32_t)good : 0));
    } else if (!strcmp(v[0], "rxset") && n > 3) {           /* bench: rxset addr clear_mask set_bits: applied after every rx start (addr 0 = clear list) */
        uint32_t a = strtoul(v[1], 0, 0); if (!a) npoke = 0; else if (npoke < 8) { poke[npoke][0] = a; poke[npoke][1] = strtoul(v[2], 0, 0); poke[npoke++][2] = strtoul(v[3], 0, 0); } printf("rxset %u\n", npoke);
    } else if (!strcmp(v[0], "rxdc")) {                     /* bench: fresh RX DC calibration (as rf_cal_check) */
        extern int rf_rx_dc_cal(unsigned rows); extern void rf_rx_on(void); rf_pkt_stop(); rf_set_freq_khz(2437000); rf_rx_on(); rf_rx_dc_cal(128); rf_rx_gain(-1); printf("rxdc\n");
    } else if (!strcmp(v[0], "bleinit")) {                  /* bench: rerun the link-layer core init */
        rf_pkt_stop(); printf("bleinit %d\n", ble_hw_init());
    } else if (!strcmp(v[0], "reopen")) {                   /* bench: rf_close + rf_open */
        rf_pkt_stop(); rf_close(); printf("reopen %d\n", rf_open());
    } else if (!strcmp(v[0], "wd") && n > 1) {              /* bench: wd ms: receive watchdog timeout, 0 = off */
        extern uint32_t rf_pkt_bench_wd_ms; rf_pkt_bench_wd_ms = (uint32_t)atoi(v[1]); printf("wd\n");
    } else if (!strcmp(v[0], "rxpoke") && n > 2) {          /* bench: set one RX CS field (offset) on the next rx */
        extern uint16_t ble_hw_rx_set_off, ble_hw_rx_set_val; ble_hw_rx_set_off = (uint16_t)strtoul(v[1], 0, 0); ble_hw_rx_set_val = (uint16_t)strtoul(v[2], 0, 0); printf("rxpoke\n");
    } else if (!strcmp(v[0], "core") && n > 2) {            /* bench: OR / AND-NOT into RWBLECNTL: core or|andnot */
        *(volatile uint32_t *)0x60031000 = (*(volatile uint32_t *)0x60031000 | (uint32_t)strtoul(v[1], 0, 0)) & ~(uint32_t)strtoul(v[2], 0, 0); printf("core %08lx\n", (unsigned long)*(volatile uint32_t *)0x60031000);
    } else if (!strcmp(v[0], "txflip") && n > 2) {          /* bench: XOR bits into one send CS field */
        extern uint16_t ble_hw_tx_flip_off, ble_hw_tx_flip_bits; ble_hw_tx_flip_off = (uint16_t)strtoul(v[1], 0, 0); ble_hw_tx_flip_bits = (uint16_t)strtoul(v[2], 0, 0); printf("txflip\n");
    } else if (!strcmp(v[0], "gainsweep") && n > 4) {       /* gainsweep mhz sync from to: forced RX gain code sweep, good packets per 100 ms */
        rf_pkt_cfg_t c = { (uint16_t)atoi(v[1]), 0, 0, (uint32_t)strtoul(v[2], 0, 0), 0x555555, 0 };
        rf_pkt_rx_start(&c, 127);
        static rf_pkt_t p;
        for (int g = atoi(v[3]); g <= atoi(v[4]); g++) {
            rf_pbus_write(1, 2, (uint16_t)g);
            uint32_t t0 = ccount(), good = 0, bad = 0;
            while (ccount() - t0 < 24000000u) if (rf_pkt_read(&p) && !(p.status & 1)) { if (p.ok) good++; else bad++; }
            printf("g %d %lu %lu\n", g, (unsigned long)good, (unsigned long)bad);
        }
        rf_pkt_stop();
    } else if (!strcmp(v[0], "pm") && n > 2) {              /* bench: pm idx and_mask -> pbus-memory word idx = default & mask */
        unsigned i = (unsigned)atoi(v[1]); uint32_t w = rf_pbus_mem_default(i) & (uint32_t)strtoul(v[2], 0, 0);
        rf_pbus_mem_write(i, w); printf("pm %u %08lx\n", i, (unsigned long)w);
    } else if (!strcmp(v[0], "txp") && n > 1) {             /* bench: send CS+0x18 (TX power index) for following sends */
        txpower = (uint8_t)strtoul(v[1], 0, 0); printf("txp\n");
    } else if (!strcmp(v[0], "rxdump") && n > 1) {          /* bench: start rx (2440 MHz, 1M) and dump the BB/AGC + FE blocks as 'r addr val' */
        rf_pkt_cfg_t c = { 2440, (uint8_t)atoi(v[1]), 0, 0x71764129, 0x555555, 0 };
        rf_pkt_rx_start(&c, -1); delay_ms(50);
        static const struct { uint32_t b, n; } bl[] = { { 0x6001C000, 0x2000 }, { 0x60006000, 0x200 }, { 0x60005000, 0x1000 }, { 0x6000E000, 0x200 }, { 0x60031000, 0x200 } };
        for (unsigned k = 0; k < 5; k++) for (uint32_t a = bl[k].b; a < bl[k].b + bl[k].n; a += 4) printf("r %08lx %08lx\n", (unsigned long)a, (unsigned long)*(volatile uint32_t *)a);
        static const uint8_t ab[] = { 0x61, 0x62, 0x63, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D };
        for (unsigned b = 0; b < sizeof ab; b++) for (int r = 0; r < 32; r++) printf("a %02x %02x %02x\n", ab[b], r, rf_regi2c_read(ab[b], (uint8_t)r));
        extern uint16_t ble_hw_em16(uint32_t off);
        for (uint32_t o = 0; o < 0x1500; o += 2) printf("e %04lx %04x\n", (unsigned long)o, ble_hw_em16(o));   /* event table, CS, ring, TX descriptors */
        rf_pkt_stop();
    } else if (!strcmp(v[0], "wr") && n > 2) {
        *(volatile uint32_t *)strtoul(v[1], 0, 0) = (uint32_t)strtoul(v[2], 0, 0); printf("wr\n");
    } else if (!strcmp(v[0], "rd") && n > 1) {              /* rd addr [count]: read count words from addr */
        uint32_t a = strtoul(v[1], 0, 0), cnt = n > 2 ? strtoul(v[2], 0, 0) : 1;
        for (uint32_t i = 0; i < cnt; i++) printf("r %08lx %08lx\n", (unsigned long)(a + 4 * i), (unsigned long)*(volatile uint32_t *)(a + 4 * i));
    } else if (!strcmp(v[0], "forced") && n > 1) {
        extern int rf_pkt_bench_forced; rf_pkt_bench_forced = atoi(v[1]) == 1; printf("forced\n");
    } else if (!strcmp(v[0], "tone") && n > 2) {            /* tone khz offset_khz: CW at khz + offset (rf.h) */
        rf_pkt_stop(); rf_tx_stop();
        int rc = rf_set_freq_khz((uint32_t)atoi(v[1])); if (!rc) rc = rf_tx_start(); if (!rc) rc = rf_tx_tone_khz(atoi(v[2]), n > 3 ? atoi(v[3]) : 300);
        printf("tone rc %d\n", rc);
    } else if (!strcmp(v[0], "tonerx") && n > 2) {          /* tonerx khz guess_hz: measured tone frequency from the LO, Hz */
        rf_pkt_stop(); rf_tx_stop();
        int rc = rf_set_freq_khz((uint32_t)atoi(v[1])); if (!rc) rc = rf_rx_start();
        float hz = 0; if (!rc) rc = rf_rx_tone_hz((float)atof(v[2]), &hz);
        printf("tonerx rc %d hz %.0f\n", rc, (double)hz);
    } else if (!strcmp(v[0], "decbench") && n > 1) {        /* decbench decim: decimator cycles per input sample (8192 samples, internal RAM) */
        extern uint32_t rf_dec_bench(uint32_t, const uint32_t *, uint32_t, int16_t *, uint32_t);
        uint32_t c = rf_dec_bench(strtoul(v[1], 0, 0), (const uint32_t *)0x3FCB0000, 4096, sbuf, 512); printf("decbench %.2f cycles/sample\n", c / 4096.0);
    } else if (!strcmp(v[0], "decsyn") && n > 2) {          /* decsyn decim f_khz: synthetic 10-bit tone through the decimator, output tone level dB */
        extern uint32_t rf_dec_bench(uint32_t, const uint32_t *, uint32_t, int16_t *, uint32_t);
        uint32_t D = strtoul(v[1], 0, 0); float f = (float)atof(v[2]) * 1e3f; uint32_t *w = (uint32_t *)0x3FCB0000;   /* dump area: free here */
        for (uint32_t k = 0; k < 8192; k++) { float ph = 2 * 3.14159265f * f * (float)k / 16e6f; int i = (int)lrintf(400 * cosf(ph)), q = (int)lrintf(-400 * sinf(ph)); w[k] = ((uint32_t)i & 0x3FF) | ((uint32_t)q & 0x3FF) << 10; }
        uint32_t nout = 8192 / D; rf_dec_bench(D, w, 8192, sbuf, nout > 512 ? 512 : nout);
        float fo = f - 16e6f / (float)D * floorf(f / (16e6f / (float)D) + 0.5f); double re = 0, im = 0; uint32_t m = nout > 512 ? 512 : nout;
        for (uint32_t k = 64; k < m; k++) { double ph = 2 * 3.14159265358979 * fo * (double)k / (16e6 / D); re += sbuf[2 * k] * cos(ph) + sbuf[2 * k + 1] * sin(ph); im += sbuf[2 * k + 1] * cos(ph) - sbuf[2 * k] * sin(ph); }
        printf("decsyn %.1f dB\n", 20 * log10(sqrt(re * re + im * im) / (double)(m - 64) / 400.0 + 1e-12));
    } else if (!strcmp(v[0], "gfsk") && n > 6) {            /* gfsk mhz rate_kbps dev_khz len amp gap_us: looping GFSK frame from the DAC */
        int ch = chan_of_mhz(atoi(v[1]));
        printf("gfsk %d\n", ch < 0 ? -9 : gfsk_play((uint32_t)atoi(v[1]) * 1000, ch, (float)atof(v[2]) * 1e3f, (float)atof(v[3]) * 1e3f, atoi(v[4]), atoi(v[5]), (float)atof(v[6])));
    } else if (!strcmp(v[0], "deccap") && n > 4) {          /* deccap khz decim n gain_db: decimated capture (rf_rx_capture_dec), hex I,Q int16 */
        rf_pkt_stop(); rf_tx_stop();
        int16_t *dbuf = sbuf; uint32_t cnt = strtoul(v[3], 0, 0);
        if (cnt > 512) { if (!psram_size() && !psram_init(2)) { printf("deccap no psram\n"); return; } dbuf = (int16_t *)PSRAM_VADDR; }   /* long runs: PSRAM */
        rf_set_freq_khz((uint32_t)atoi(v[1])); rf_rx_start(); rf_rx_set_gain_db(atoi(v[4])); delay_us(200);
        uint32_t t0 = ccount(); int rc = rf_rx_capture_dec(strtoul(v[2], 0, 0), cnt, dbuf); uint32_t us = (ccount() - t0) / 240;
        extern uint32_t rf_cap_last_late; printf("deccap rc %d us %lu late %lu\n", rc, (unsigned long)us, (unsigned long)rf_cap_last_late);
        for (uint32_t k = 0; k < cnt && k < 1024; k += 16) { for (uint32_t j = k; j < k + 16 && j < cnt; j++) printf("%04x%04x", (uint16_t)dbuf[2 * j], (uint16_t)dbuf[2 * j + 1]); printf("\n"); }
    } else if (!strcmp(v[0], "iqcap") && n > 1) {           /* iqcap khz [target]: raw IQ (AGC; target > 0: autogain to that peak; target < 0: gain -target dB held), 16 MS/s, 16383 words, printed as hex */
        rf_pkt_stop(); rf_tx_stop();
        rf_set_freq_khz((uint32_t)atoi(v[1])); rf_rx_start(); if (n < 3 || atoi(v[2]) >= 0) rf_rx_set_gain(RF_GAIN_AGC); int db = -1; if (n > 2 && atoi(v[2]) > 0) rf_rx_autogain(atoi(v[2]), &db); else if (n > 2 && atoi(v[2]) < 0) { db = -atoi(v[2]); rf_rx_set_gain_db(db); delay_us(200); }
        const uint32_t *iq; int rc = rf_rx_capture(RF_RATE_16M, RF_CAPTURE_MAX, &iq);
        printf("iqcap rc %d gain %d\n", rc, db);
        for (uint32_t k = 0; k < RF_CAPTURE_MAX; k += 16) { for (uint32_t j = k; j < k + 16 && j < RF_CAPTURE_MAX; j++) printf("%05lx", (unsigned long)(iq[j] & 0xFFFFF)); printf("\n"); }
    } else if (!strcmp(v[0], "ptime")) {                    /* 7: power-state transition timing (us) */
        rf_pkt_stop(); rf_tx_stop(); rf_close();
        uint32_t a = ccount(); rf_open();           uint32_t t_open = (ccount() - a) / 240;
        rf_set_freq_khz(2437000);
        a = ccount(); rf_rx_start();                uint32_t t_rx = (ccount() - a) / 240;
        rf_pkt_stop();
        a = ccount(); rf_tx_start();                uint32_t t_tx = (ccount() - a) / 240;
        rf_tx_stop();
        a = ccount(); rf_close();                   uint32_t t_close = (ccount() - a) / 240;
        rf_open(); rf_set_freq_khz(2437000);         /* leave it ready */
        printf("ptime off->idle %lu us, idle->rx %lu us, idle->tx %lu us, ->off %lu us\n",
               (unsigned long)t_open, (unsigned long)t_rx, (unsigned long)t_tx, (unsigned long)t_close);
    } else if (!strcmp(v[0], "corr")) {                     /* 4.4: capture at 2405 MHz, correlator-trigger on the 15.4 preamble */
        rf_pkt_stop(); rf_tx_stop();
        rf_set_freq_khz(2405000); rf_rx_start(); rf_rx_set_gain(RF_GAIN_AGC);
        const uint32_t *iq; int rc = rf_rx_capture(RF_RATE_16M, RF_CAPTURE_MAX, &iq);
        int8_t *rI = (int8_t *)sbuf, *rQ = rI + 1024; const uint32_t pn0 = 0xD9C3522Eu;   /* 8-symbol preamble (symbol 0) */
        for (int k = 0; k < 1024; k++) rI[k] = rQ[k] = 0;
        for (int sy = 0; sy < 4; sy++) for (int c = 0; c < 32; c++) {
            int s = (pn0 >> c) & 1 ? 1 : -1; int8_t *d = (c & 1) ? rQ : rI; int base = sy * 256 + c * 8;
            for (int t = 0; t < 16 && base + t < 1024; t++) d[base + t] = (int8_t)(s * 100 * sinf((float)M_PI * t / 16));
        }
        uint32_t off; int64_t mag; int32_t r = rf_corr_trigger(iq, RF_CAPTURE_MAX, rI, rQ, 1024, &off, &mag);
        printf("corr rc %d trigger_off %lu ratio %ld.%02ld\n", rc, (unsigned long)off, (long)(r / 256), (long)((r % 256) * 100 / 256));
    } else if (!strcmp(v[0], "awr") && n > 3) {             /* awr block reg val: analog register write */
        rf_regi2c_write((uint8_t)strtoul(v[1], 0, 16), (uint8_t)strtoul(v[2], 0, 16), (uint8_t)strtoul(v[3], 0, 16)); printf("awr\n");
    } else if (!strcmp(v[0], "adv") && n > 2) {             /* adv interval_us txpwr: hardware ADV_NONCONN_IND "NDF" on ch 37 (reference for the IDF scanner) */
        static int bi; if (!bi) { ble_hw_init(); bi = 1; }
        static const uint8_t a[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 }, d[] = { 2, 1, 6, 4, 9, 'N', 'D', 'F' };
        int rc = ble_hw_adv(37, a, 1, d, sizeof d, (uint32_t)atoi(v[1]));
        ble_hw_em_wr(0x418, (uint16_t)((ble_hw_em_rd(0x418) & 0xFF00) | (atoi(v[2]) & 0xFF)));
        printf("adv rc %d\n", rc);
    } else if (!strcmp(v[0], "gaincurve") && n > 2) {       /* gaincurve khz tone_hz: tone level (dB) at every RX gain index + the gain-memory words */
        rf_pkt_stop(); rf_tx_stop();
        rf_set_freq_khz((uint32_t)atoi(v[1])); rf_rx_start();
        float f = (float)atof(v[2]);
        extern uint32_t rf_gain_shadow[256][2];
        for (int g = 0; g < 128; g++) {
            rf_rx_set_gain(g); delay_us(200);
            const uint32_t *iq; rf_rx_capture(RF_RATE_40M, 4000, &iq);
            float c = cosf(2 * 3.14159265f * f / 40e6f), sn = sinf(2 * 3.14159265f * f / 40e6f), re = 1, im = 0, ai = 0, aq = 0; double pw = 0; int pk = 0;
            for (int k = 0; k < 4000; k++) {
                float i = (float)rf_sample_i(iq[k]), q = (float)rf_sample_q(iq[k]);
                ai += i * re + q * im; aq += q * re - i * im;
                float t = re * c - im * sn; im = re * sn + im * c; re = t;
                pw += (double)i * i + (double)q * q; if (abs((int)i) > pk) pk = abs((int)i); if (abs((int)q) > pk) pk = abs((int)q);
            }
            float tone = 10 * log10f((ai * ai + aq * aq) / 16e6f + 1e-9f), tot = 10 * log10f((float)(pw / 4000) + 1e-9f);
            printf("g %3d tone %6.1f total %6.1f peak %3d mem %08lx %08lx\n", g, (double)tone, (double)tot, pk, (unsigned long)rf_gain_shadow[g][0], (unsigned long)rf_gain_shadow[g][1]);
        }
        rf_rx_set_gain(RF_GAIN_AGC);
    } else if (!strcmp(v[0], "agc") && n > 1) {             /* agc khz: AGC on, read 0x6001C02C a few times + capture peak / tone level at +1 MHz */
        rf_pkt_stop(); rf_tx_stop();
        rf_set_freq_khz((uint32_t)atoi(v[1])); rf_rx_start(); rf_rx_set_gain(RF_GAIN_AGC); delay_ms(5);
        printf("agc reg");
        for (int k = 0; k < 6; k++) { printf(" %08lx", (unsigned long)*(volatile uint32_t *)0x6001C02C); delay_us(300); }
        const uint32_t *iq; rf_rx_capture(RF_RATE_40M, 4000, &iq); int pk = 0; double pw = 0;
        for (int k = 0; k < 4000; k++) { int i = rf_sample_i(iq[k]), q = rf_sample_q(iq[k]); pw += i * i + q * q; if (abs(i) > pk) pk = abs(i); if (abs(q) > pk) pk = abs(q); }
        printf(" | peak %d total %.1f dB\n", pk, 10 * log10(pw / 4000 + 1e-9));
    } else if (!strcmp(v[0], "autogain") && n > 2) {        /* autogain khz target: rf_rx_autogain, then the capture peak */
        rf_pkt_stop(); rf_tx_stop(); rf_set_freq_khz((uint32_t)atoi(v[1])); rf_rx_start();
        int db = -1; uint32_t t0 = ccount(); int rc = rf_rx_autogain(atoi(v[2]), &db); uint32_t t1 = ccount();
        const uint32_t *iq; rf_rx_capture(RF_RATE_40M, 4000, &iq); int pk = 0;
        for (int k = 0; k < 4000; k++) { int i = abs(rf_sample_i(iq[k])), q = abs(rf_sample_q(iq[k])); if (i > pk) pk = i; if (q > pk) pk = q; }
        printf("autogain rc %d db %d peak %d in %lu us\n", rc, db, pk, (unsigned long)((t1 - t0) / 240));
    } else if (!strcmp(v[0], "image") && n > 2) {          /* image khz tone_hz: autogain, then tone and image (-tone) power, dB */
        rf_pkt_stop(); rf_tx_stop(); rf_set_freq_khz((uint32_t)atoi(v[1])); rf_rx_start();
        int db; rf_rx_autogain(200, &db);
        float f = (float)atof(v[2]); double a[2] = { 0, 0 };
        for (int rep = 0; rep < 4; rep++) {
            const uint32_t *iq; rf_rx_capture(RF_RATE_40M, 8000, &iq);
            for (int sgn = 0; sgn < 2; sgn++) {
                float w = (sgn ? -1 : 1) * 2 * 3.14159265f * f / 40e6f, c = cosf(w), sn = sinf(w), re = 1, im = 0, ai = 0, aq = 0;
                for (int k = 0; k < 8000; k++) { float i = (float)rf_sample_i(iq[k]), q = (float)rf_sample_q(iq[k]); ai += i * re + q * im; aq += q * re - i * im; float t = re * c - im * sn; im = re * sn + im * c; re = t; }
                a[sgn] += ((double)ai * ai + (double)aq * aq);
            }
        }
        printf("image gain %d tone %.1f image %.1f rejection %.1f dB\n", db, 10 * log10(a[0] / 4 / 64e6), 10 * log10(a[1] / 4 / 64e6), 10 * log10(a[0] / a[1]));
    } else if (!strcmp(v[0], "lead") && n > 1) {
        extern uint32_t ble_hw_raw_lead, ble_hw_raw_jitter; ble_hw_raw_lead = (uint32_t)atoi(v[1]); ble_hw_raw_jitter = n > 2 ? (uint32_t)atoi(v[2]) : 0; printf("lead\n");
    } else if (!strcmp(v[0], "level") && n > 1) {           /* level db: rf_tx_set_level_db on the running tone */
        printf("level rc %d\n", rf_tx_set_level_db(atoi(v[1])));
    } else if (!strcmp(v[0], "tonedb") && n > 3) {          /* tonedb khz tone_hz gain_db: tone level at a fixed RX gain, mean of 8 captures */
        rf_pkt_stop(); rf_tx_stop(); rf_set_freq_khz((uint32_t)atoi(v[1])); rf_rx_start(); rf_rx_set_gain_db(atoi(v[3])); delay_us(200);
        float f = (float)atof(v[2]); double acc = 0; int pk = 0;
        for (int rep = 0; rep < 8; rep++) {
            const uint32_t *iq; rf_rx_capture(RF_RATE_40M, 4000, &iq);
            float w = 2 * 3.14159265f * f / 40e6f, c = cosf(w), sn = sinf(w), re = 1, im = 0, ai = 0, aq = 0;
            for (int k = 0; k < 4000; k++) { float i = (float)rf_sample_i(iq[k]), q = (float)rf_sample_q(iq[k]); ai += i * re + q * im; aq += q * re - i * im; float t = re * c - im * sn; im = re * sn + im * c; re = t; if (abs((int)i) > pk) pk = abs((int)i); }
            acc += ((double)ai * ai + (double)aq * aq) / 16e6;
        }
        printf("tonedb %.2f peak %d\n", 10 * log10(acc / 8), pk);
    } else if (!strcmp(v[0], "dig") && n > 1) {             /* dig code: raw TX digital gain byte on the running tone */
        extern void rf_tx_gain(uint8_t gain, uint16_t bb, int8_t dig); rf_tx_gain(127, 0, (int8_t)strtol(v[1], 0, 0)); printf("dig\n");
    } else if (!strcmp(v[0], "txdc")) {                     /* txdc: our TX LO-leakage calibration on the running tone */
        uint32_t t0 = ccount(); rf_tx_dc_cal(127, 0); printf("txdc %lu us\n", (unsigned long)((ccount() - t0) / 240));
    } else if (!strcmp(v[0], "ard") && n > 2) {
        printf("ard %02x\n", rf_regi2c_read((uint8_t)strtoul(v[1], 0, 16), (uint8_t)strtoul(v[2], 0, 16)));
    } else if (!strcmp(v[0], "pbw") && n > 3) {             /* pbw row col value: one PHY-bus word */
        rf_pbus_write((uint8_t)atoi(v[1]), (uint8_t)atoi(v[2]), (uint16_t)strtoul(v[3], 0, 0)); printf("pbw\n");
    } else if (!strcmp(v[0], "sched") && n > 3) {           /* sched mhz count period_us: packets at exact times on the radio clock */
        rf_pkt_cfg_t c = { (uint16_t)atoi(v[1]), 0, 0, 0x71764129, 0x555555, 0 };
        rf_pkt_send(&c, 0x5A, payload, 37, 1); while (rf_pkt_busy()) {}               /* sets up the core: time base valid */
        uint32_t cnt = (uint32_t)atoi(v[2]), per = (uint32_t)atoi(v[3]), late = 0;
        uint64_t t0 = rf_time_us() + 2000;
        for (uint32_t k = 0; k < cnt; k++) {
            while (rf_pkt_busy()) {}
            if (rf_pkt_send_at(&c, 0x5A, payload, 37, t0 + (uint64_t)k * per)) late++;
        }
        while (rf_pkt_busy()) {}
        printf("sched sent %lu late %lu\n", (unsigned long)(cnt - late), (unsigned long)late);
    } else if (!strcmp(v[0], "stop")) {
        rf_pkt_stop(); rf_tx_stop(); printf("stop\n");
    } else if (!strcmp(v[0], "limits") && n > 2) {
        rf_limits_t l; rf_get_limits(&l); l.tx_min_khz = (uint32_t)atoi(v[1]); l.tx_max_khz = (uint32_t)atoi(v[2]); rf_set_limits(&l); printf("limits\n");
    }
}

int main(void)
{
    delay_ms(300);
    for (int k = 0; k < 255; k++) payload[k] = (uint8_t)k;
    int rc = rf_open();
    printf("rfctl ready (rf_pkt) open %d\n", rc);
    int k = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch == '\n') { line[k] = 0; command(); printf("ok\n"); k = 0; continue; }
        if (k < (int)sizeof line - 1) line[k++] = (char)ch;
    }
}
