/* rf_pkt: the hardware packet engine (rf.h) driven by line commands, for tools/rf_pkt_test.py (two boards).
 *   send mhz rate sync flags len count     payload 00 01 02 ...; count 0 = continuous; prints packets/s over 1 s
 *   rx mhz rate sync flags gain ms         receive for ms: good / CRC-bad packets, wrong bytes, RSSI, CPU
 *   stop | limits min_khz max_khz */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include "rf.h"
#include "ble_hw.h"
#include "../../drivers/rf/rf_init.h"
#include "../../drivers/rf/rf_regi2c.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static char line[96];
static uint8_t payload[255];
static uint8_t txpower;                            /* 0 = default */

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
        static rf_pkt_t p; uint32_t good = 0, crc = 0, wrong = 0, lens = 0, t0 = ccount(), dur = (uint32_t)atoi(v[6]) * 240000u; int32_t rs = 0; uint8_t hdr = 0; uint32_t crcdata = 0, empty = 0; int rmin = 127, rmax = -128;
        while (ccount() - t0 < dur) {
            if (!rf_pkt_read(&p)) continue;
            if (p.status & 1) continue;                /* window ended without a packet */
            if (!p.len) { empty++; continue; }
            if (!p.ok) { crc++; unsigned m = 0; while (m < p.len && p.data[m] == (uint8_t)m) m++; if (p.len && m == p.len) crcdata++; continue; }
            good++; rs += p.rssi; lens = p.len; hdr = p.hdr; if (p.rssi < rmin) rmin = p.rssi; if (p.rssi > rmax) rmax = p.rssi;
            for (unsigned k = 0; k < p.len; k++) if (p.data[k] != (uint8_t)k) { wrong++; break; }
        }
        uint32_t ctl = *(volatile uint32_t *)0x60031000;
        rf_pkt_stop();
        printf("rx ctl %08lx\n", (unsigned long)ctl);
        printf("rx empty %lu rssi_min %d rssi_max %d\n", (unsigned long)empty, rmin, rmax);
        printf("rx rc 0 good %lu crcbad %lu crcbad_dataok %lu wrong %lu len %lu hdr %02x rssi %ld\n", (unsigned long)good, (unsigned long)crc, (unsigned long)crcdata, (unsigned long)wrong, (unsigned long)lens, hdr, (long)(good ? rs / (int32_t)good : 0));
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
        static const struct { uint32_t b, n; } bl[] = { { 0x6001C000, 0x2000 }, { 0x60006000, 0x200 }, { 0x60005000, 0x1000 }, { 0x6000E000, 0x200 } };
        for (unsigned k = 0; k < 4; k++) for (uint32_t a = bl[k].b; a < bl[k].b + bl[k].n; a += 4) printf("r %08lx %08lx\n", (unsigned long)a, (unsigned long)*(volatile uint32_t *)a);
        static const uint8_t ab[] = { 0x61, 0x62, 0x63, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D };
        for (unsigned b = 0; b < sizeof ab; b++) for (int r = 0; r < 32; r++) printf("a %02x %02x %02x\n", ab[b], r, rf_regi2c_read(ab[b], (uint8_t)r));
        rf_pkt_stop();
    } else if (!strcmp(v[0], "wr") && n > 2) {
        *(volatile uint32_t *)strtoul(v[1], 0, 0) = (uint32_t)strtoul(v[2], 0, 0); printf("wr\n");
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
    } else if (!strcmp(v[0], "iqcap") && n > 1) {           /* iqcap khz: raw IQ, 16 MS/s, 16383 words, printed as hex */
        rf_pkt_stop(); rf_tx_stop();
        rf_set_freq_khz((uint32_t)atoi(v[1])); rf_rx_start(); rf_rx_set_gain(RF_GAIN_AGC);
        const uint32_t *iq; int rc = rf_rx_capture(RF_RATE_16M, RF_CAPTURE_MAX, &iq);
        printf("iqcap rc %d\n", rc);
        for (uint32_t k = 0; k < RF_CAPTURE_MAX; k += 16) { for (uint32_t j = k; j < k + 16 && j < RF_CAPTURE_MAX; j++) printf("%05lx", (unsigned long)(iq[j] & 0xFFFFF)); printf("\n"); }
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
    } else if (!strcmp(v[0], "stop")) {
        rf_pkt_stop(); printf("stop\n");
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
