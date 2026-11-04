/* rfctl: the radio driven by line commands over the console, for host-scripted
 * measurements (tools/rf/rfhost.py). Every command answers with result lines and
 * then "ok" (or "err ..."). Frequencies in Hz are in the true spectrum. */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include "tsens.h"
#include "regi2c.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "rf_regi2c.h"
#include "rf_pll.h"
#include "rf_init.h"
#include "rf_dump.h"

#define R(a) (*(volatile uint32_t *)(a))

static int hann;                                /* "hann 1": Hann-window the bins (spur / skirt work), 0 dB = same tone as rectangular */
static float bin_db(const uint32_t *w, uint32_t n, float f, float fs)
{
    float c = cosf(2 * (float)M_PI * f / fs), s = sinf(2 * (float)M_PI * f / fs), re = 0, im = 0, cr = 1, ci = 0;
    for (uint32_t k = 0; k < n; k++) {
        float i = rf_iq_i(w[k]), q = -rf_iq_q(w[k]);
        if (hann) { float h = 1 - cosf(2 * (float)M_PI * k / n); i *= h; q *= h; }
        re += i * cr + q * ci; im += q * cr - i * ci;
        float t = cr * c - ci * s; ci = ci * c + cr * s; cr = t;
        if ((k & 255) == 255) { float m = 1.0f / sqrtf(cr * cr + ci * ci); cr *= m; ci *= m; }
    }
    return 10 * log10f((re * re + im * im) / ((float)n * n) + 1e-6f);
}

static int tx_active;
static void tx_tone(int gain, int bb, int if_khz, int amp)
{
    const uint32_t n = 2000;                    /* 40 MS/s: if_khz must divide into 20 kHz steps */
    volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1);
    if (tx_active) { rf_dac_stop(); }
    for (uint32_t k = 0; k < n; k++) {
        float ph = 2 * (float)M_PI * (float)if_khz * 1000.0f * (float)k / 40e6f;
        b[k] = rf_iq_word((int)lrintf(amp * cosf(ph)), (int)lrintf(amp * sinf(ph)));
    }
    rf_tx_on((uint8_t)gain, (uint16_t)bb);
    rf_dac_play(1, n, 0, 1);
    tx_active = 1;
}

/* BLE 1M advertising in software (A.5): ADV_NONCONN_IND with a complete local name, CRC-24 and whitening per
 * the core spec (byte-wise forms from dmitry.gr "BLE fun"), GFSK BT 0.5, deviation 250 kHz, 40 samples per bit
 * at 40 MS/s, on an IF offset from the LO. The bank loops: packet, then silence, 400 us per period. */
static uint8_t swapbits(uint8_t a) { uint8_t v = 0; for (int k = 0; k < 8; k++) if (a & (1 << k)) v |= 0x80 >> k; return v; }
static int ble_build(uint8_t *pkt, int chan, const char *name)
{
    static const uint8_t adva[6] = { 0x01, 0x02, 0x03, 0x0D, 0x0F, 0xC0 };   /* random static, LSB first */
    int nl = (int)strlen(name), n = 0;
    pkt[n++] = 0xAA; pkt[n++] = 0xD6; pkt[n++] = 0xBE; pkt[n++] = 0x89; pkt[n++] = 0x8E;
    uint8_t *pdu = pkt + n;
    pdu[0] = 0x42; pdu[1] = (uint8_t)(6 + 3 + 2 + nl);               /* ADV_NONCONN_IND, TxAdd random */
    memcpy(pdu + 2, adva, 6);
    pdu[8] = 2; pdu[9] = 1; pdu[10] = 6;
    pdu[11] = (uint8_t)(nl + 1); pdu[12] = 9; memcpy(pdu + 13, name, (size_t)nl);
    int pl = 13 + nl;
    uint8_t c[3] = { 0x55, 0x55, 0x55 };
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
    for (int k = 0; k < pl + 3; k++)
        for (uint8_t m = 1; m; m <<= 1) { if (wc & 0x80) { wc ^= 0x11; pdu[k] ^= m; } wc <<= 1; }
    return n + pl + 3;
}

static void ble_adv(int chan, int if_khz, int amp, const char *name)
{
    static uint8_t pkt[64];
    int nb = ble_build(pkt, chan, name) * 8;
    const int sps = 40, half = 3 * sps;
    static float g[2 * 3 * 40 + 1]; float gs = 0;
    for (int k = -half; k <= half; k++) {                           /* Gaussian, sigma = sqrt(ln2) / (2 pi BT) bits */
        float t = (float)k / sps / 0.2650f; g[k + half] = expf(-0.5f * t * t); gs += g[k + half];
    }
    const uint32_t n = 16000;
    volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1);
    if (tx_active) rf_dac_stop();
    float ph = 0;
    for (uint32_t k = 0; k < n; k++) {
        float f = 0;                                                /* filtered NRZ, -1..1 */
        int on = (int)k < (nb + 6) * sps;
        if (on) for (int j = -half; j <= half; j += 2) {           /* every 2nd tap: 40 samples/bit is plenty */
            int s = (int)k - 3 * sps + j, bit = s / sps;
            float v = (s < 0 || bit >= nb) ? 0 : ((pkt[bit >> 3] >> (bit & 7)) & 1 ? 1.f : -1.f);
            f += v * g[j + half];
        }
        f *= 2 / gs;
        ph += 2 * (float)M_PI * ((float)if_khz * 1e3f + 250e3f * f) / 40e6f;
        if (ph > (float)M_PI) ph -= 2 * (float)M_PI;
        if (ph < -(float)M_PI) ph += 2 * (float)M_PI;
        b[k] = on ? rf_iq_word((int)lrintf(amp * cosf(ph)), (int)lrintf(amp * sinf(ph))) : rf_iq_word(0, 0);
    }
    rf_tx_on(127, 0);
    rf_dac_play(1, n, 0, 1);
    tx_active = 1;
    printf("bleadv %d bits\n", nb);
}

/* BLE 1M receive in software: 16 MS/s bursts (1 ms each), skip bursts without energy, FM discriminator by
 * conjugate product with the +2 MHz IF removed (sign only), access-address search at mid-bit, dewhiten,
 * CRC-24 check. LO = channel - 2 MHz. Returns the try that decoded, or -1. */
static int8_t ble_d[RF_DUMP_MAX];                              /* normalised discriminator, x100 */
static int BLE_BIT(int k) { int a = 0; for (int j = 0; j < 16; j++) a += ble_d[k + j]; return a > 0; }   /* integrate and dump */
static int ble_rx(int chan, int tries, int qs)
{
    const int sps = 16; const float c4 = 0.70710678f;            /* IF step pi/4 per sample */
    int hot = 0, best = 99, besti = 99, ones = 0;
    for (int t = 0; t < tries; t++) {
        rf_dump_burst(RF_DUMP_16M, RF_DUMP_MAX);
        const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
        float pmax = 0, pmin = 1e30f;
        for (int b = 0; b < (int)RF_DUMP_MAX / 256; b++) {
            float p = 0;
            for (int k = b * 256; k < b * 256 + 256; k += 4) { float i = rf_iq_i(w[k]), q = rf_iq_q(w[k]); p += i * i + q * q; }
            if (p > pmax) pmax = p;
            if (p < pmin) pmin = p;
        }
        if (pmax < 10 * pmin) continue;
        hot++;
        int pi = rf_iq_i(w[0]), pq = qs * rf_iq_q(w[0]);
        for (int k = 1; k < (int)RF_DUMP_MAX; k++) {
            int i = rf_iq_i(w[k]), q = qs * rf_iq_q(w[k]);
            float re = (float)i * pi + (float)q * pq, im = (float)q * pi - (float)i * pq;
            float d = im * c4 - re * c4, m = sqrtf(re * re + im * im) + 1e-3f;   /* Im(x * e^-j pi/4), normalised */
            ble_d[k] = (int8_t)lrintf(100 * d / m);
            ones += d > 0;
            pi = i; pq = q;
        }
        const uint32_t aa = 0x8E89BED6u;
        for (int k0 = 1; k0 + sps * (40 + 8 * 42) < (int)RF_DUMP_MAX; k0++) {
            int err = 0;
            for (int m = 0; m < 32; m++) err += BLE_BIT(k0 + sps * (8 + m)) != (int)((aa >> m) & 1);
            if (err < best) best = err;
            if (32 - err < besti) besti = 32 - err;
            if (err > 1) continue;
            uint8_t pdu[42] = { 0 };
            uint8_t wc = swapbits((uint8_t)chan) | 2;
            for (int by = 0; by < 42; by++)
                for (int bi = 0; bi < 8; bi++) {
                    int v = BLE_BIT(k0 + sps * (40 + 8 * by + bi));
                    if (wc & 0x80) { wc ^= 0x11; v ^= 1; }
                    wc <<= 1;
                    pdu[by] |= (uint8_t)(v << bi);
                }
            int len = pdu[1] & 0x3F;
            if (len > 37) continue;
            uint8_t c[3] = { 0x55, 0x55, 0x55 };
            for (int k = 0; k < len + 2; k++) {
                uint8_t d = pdu[k];
                for (int v = 0; v < 8; v++, d >>= 1) {
                    uint8_t tb = c[0] >> 7;
                    c[0] = (uint8_t)(c[0] << 1 | c[1] >> 7); c[1] = (uint8_t)(c[1] << 1 | c[2] >> 7); c[2] <<= 1;
                    if (tb != (d & 1)) { c[2] ^= 0x5B; c[1] ^= 0x06; }
                }
            }
            int ok = 1;
            for (int k = 0; k < 3; k++) ok &= pdu[len + 2 + k] == swapbits(c[k]);
            if (!ok) continue;
            printf("blerx try %d at %d type %x len %d crc ok pdu", t, k0, pdu[0] & 0xF, len);
            for (int k = 0; k < len + 2; k++) printf(" %02x", pdu[k]);
            printf("\n");
            return t;
        }
    }
    printf("blerx none in %d tries (%d with energy, AA errors best %d, inverted %d, ones %d)\n", tries, hot, best, besti, ones);
    return -1;
}

static uint32_t txs_tab[16];                             /* one period of a 2.5 MHz tone (16 samples at 40 MS/s; divides the 8192-word unit) */
static int txs_cb(uint32_t *buf, uint32_t n, void *ctx)   /* repeats the period: ~2 cycles/word (models a buffer/PSRAM source) */
{
    (void)ctx;
    const uint32_t *t = txs_tab;
    for (uint32_t k = 0; k < n; k += 16) {
        buf[k] = t[0]; buf[k + 1] = t[1]; buf[k + 2] = t[2]; buf[k + 3] = t[3]; buf[k + 4] = t[4]; buf[k + 5] = t[5]; buf[k + 6] = t[6]; buf[k + 7] = t[7];
        buf[k + 8] = t[8]; buf[k + 9] = t[9]; buf[k + 10] = t[10]; buf[k + 11] = t[11]; buf[k + 12] = t[12]; buf[k + 13] = t[13]; buf[k + 14] = t[14]; buf[k + 15] = t[15];
    }
    return 0;
}

static char line[256];
static int argv_n(char **v, int max)
{
    int n = 0; char *p = line;
    while (*p && n < max) { while (*p == ' ') p++; if (!*p) break; v[n++] = p; while (*p && *p != ' ') p++; if (*p) *p++ = 0; }
    return n;
}
static long num(const char *s) { return strtol(s, 0, 0); }

static void command(void)
{
    char *v[24]; int n = argv_n(v, 24);
    if (!n) return;
    const char *c = v[0];
    if (!strcmp(c, "init")) {
        uint32_t t = ccount(); rf_init_replay(); rf_rx_on();
        printf("init %lu us\n", (unsigned long)((ccount() - t) / (sys_cpu_hz() / 1000000)));
    } else if (!strcmp(c, "rxcal")) {
        uint32_t t = ccount(); int w = rf_rx_dc_cal(128);
        printf("rxcal %lu us worst %.1f\n", (unsigned long)((ccount() - t) / (sys_cpu_hz() / 1000000)), w / 16.0);
    } else if (!strcmp(c, "tune") && n > 1) {
        printf("tune rc %d\n", rf_tune_rx_khz((uint32_t)num(v[1])));
    } else if (!strcmp(c, "tunetx") && n > 1) {
        rf_pll_result_t r; uint32_t t = ccount(); int rc = rf_pll_set_khz((uint32_t)num(v[1]), &r);
        printf("tunetx rc %d cap %u locked %d %lu us\n", rc, r.cap, r.locked, (unsigned long)((ccount() - t) / (sys_cpu_hz() / 1000000)));
    } else if (!strcmp(c, "hop") && n > 2) {
        uint32_t t = ccount(); rf_pll_hop((uint32_t)num(v[1]), (uint16_t)num(v[2]));
        printf("hop %lu cycles\n", (unsigned long)(ccount() - t));
    } else if (!strcmp(c, "hopmeas") && n > 5) {       /* hopmeas park_khz park_cap khz cap f_hz: hop, capture at once, tone per 256 */
        rf_pll_hop((uint32_t)num(v[1]), (uint16_t)num(v[2]));
        uint32_t nn = rf_pll_n_of_hz((uint64_t)num(v[3]) * 1000u);
        delay_us(300);
        uint32_t t = ccount();
        rf_pll_hop_n(nn, (uint16_t)num(v[4]));
        uint32_t hop = ccount() - t;
        rf_dump_burst(RF_DUMP_80M, 8192);
        const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
        extern unsigned rf_pll_hop_writes;
        printf("hopmeas %lu cycles: writes %02x", (unsigned long)hop, rf_pll_hop_writes);
        for (int k = 0; k < 32; k++) printf(" %.1f", bin_db(w + 256 * k, 256, (float)atof(v[5]), 80e6f));
        printf("\n");
    } else if (!strcmp(c, "pbus") && n > 3) {
        rf_pbus_write((uint8_t)num(v[1]), (uint8_t)num(v[2]), (uint16_t)num(v[3]));
    } else if (!strcmp(c, "pmem") && n > 2) {         /* pmem idx value: one pbus-memory word (address 0x200 + idx) */
        R(0x600060CC) = (uint32_t)strtoul(v[2], 0, 0);
        R(0x600060C8) = (R(0x600060C8) & 0xFFFC00FFu) | (uint32_t)((0x200 + num(v[1])) & 0x3FF) << 8;
        R(0x600060C8) &= 0xFFFCFFFFu;
    } else if (!strcmp(c, "freq") && n > 1) {         /* freq f_hz: precise tone frequency from the phase slope over 16 blocks */
        rf_dump_burst(RF_DUMP_80M, 16384 - 1);
        const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
        float f0 = (float)atof(v[1]); double ph[16];
        for (int b = 0; b < 16; b++) {
            double re = 0, im = 0;
            for (int k = 0; k < 1023; k++) {
                int idx = b * 1023 + k; double a = -2 * M_PI * f0 * idx / 80e6, cs = cos(a), sn = sin(a);
                double i = rf_iq_i(w[idx]), q = -rf_iq_q(w[idx]);
                re += i * cs - q * sn; im += i * sn + q * cs;
            }
            ph[b] = atan2(im, re);
        }
        double s = 0; for (int b = 1; b < 16; b++) { double d = ph[b] - ph[b - 1]; while (d > M_PI) d -= 2 * M_PI; while (d < -M_PI) d += 2 * M_PI; s += d; }
        printf("freq %.1f Hz\n", f0 + s / 15 / (2 * M_PI) * 80e6 / 1023);
    } else if (!strcmp(c, "lat")) {                    /* capture start latency: start pulse -> write index moves */
        R(0x60033D5C) = 0; R(0x60033D90) = 0x000C2040u; R(0x600C101C) = 1;
        R(0x60033D5C) = 0x24000u; uint32_t i0 = R(0x60033D60) & 0x3FFF;
        uint32_t t0 = ccount(); R(0x60033D5C) = 0x24000u | 1u << 31;
        while ((R(0x60033D60) & 0x3FFF) == i0 && ccount() - t0 < 2400000) {}
        uint32_t t1 = ccount() - t0;
        R(0x60033D5C) = 0; R(0x600C101C) = 0;
        printf("lat capture run -> first index change %lu cycles = %.0f ns\n", (unsigned long)t1, t1 * 1000.0 / (sys_cpu_hz() / 1e6));
    } else if (!strcmp(c, "gain") && n > 1) {
        rf_rx_gain((int)num(v[1]));
    } else if (!strcmp(c, "row") && n > 3) {
        rf_gain_mem_write((uint8_t)num(v[1]), (uint32_t)strtoul(v[2], 0, 0), (uint32_t)strtoul(v[3], 0, 0));
    } else if (!strcmp(c, "rowget") && n > 1) {
        int i = (int)num(v[1]); printf("row %d %08lx %08lx\n", i, (unsigned long)rf_gain_shadow[i][0], (unsigned long)rf_gain_shadow[i][1]);
    } else if (!strcmp(c, "tx") && n > 4) {
        tx_tone((int)num(v[1]), (int)num(v[2]), (int)num(v[3]), (int)num(v[4]));
    } else if (!strcmp(c, "txoff")) {
        if (tx_active) { rf_dac_stop(); rf_tx_off(); tx_active = 0; }
    } else if (!strcmp(c, "meas") && n > 1) {           /* meas N [f_hz ...] ; rate 80 */
        uint32_t ns = (uint32_t)num(v[1]); if (ns > RF_DUMP_MAX) ns = RF_DUMP_MAX;
        int rc = rf_dump_burst(RF_DUMP_80M, ns);
        const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
        double si = 0, sq = 0, pp = 0; int pk = 0;
        for (uint32_t k = 0; k < ns; k++) {
            int i = rf_iq_i(w[k]), q = -rf_iq_q(w[k]);
            si += i; sq += q; pp += (double)i * i + (double)q * q;
            if (abs(i) > pk) pk = abs(i);
            if (abs(q) > pk) pk = abs(q);
        }
        double mi = si / ns, mq = sq / ns;
        printf("meas rc %d dc %.2f %.2f rms %.2f peak %d", rc, mi, mq, sqrt(pp / ns - mi * mi - mq * mq), pk);
        for (int k = 2; k < n; k++) printf(" %.1f", bin_db(w, ns, (float)atof(v[k]), 80e6f));
        printf("\n");
    } else if (!strcmp(c, "scan") && n > 3) {          /* scan N lo_hz hi_hz step_hz: strongest bin */
        uint32_t ns = (uint32_t)num(v[1]); if (ns > RF_DUMP_MAX) ns = RF_DUMP_MAX;
        rf_dump_burst(RF_DUMP_80M, ns);
        const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
        float lo = (float)atof(v[2]), hi = (float)atof(v[3]), st = n > 4 ? (float)atof(v[4]) : 100e3f, best = -200, bf = 0;
        for (float f = lo; f <= hi; f += st) { float p = bin_db(w, ns, f, 80e6f); if (p > best) { best = p; bf = f; } }
        printf("scan peak %.0f Hz %.1f dB\n", bf, best);
    } else if (!strcmp(c, "bleadv") && n > 4) {       /* bleadv chan if_khz amp name: tune the LO first (tunetx) */
        ble_adv((int)num(v[1]), (int)num(v[2]), (int)num(v[3]), v[4]);
    } else if (!strcmp(c, "blerx") && n > 2) {        /* blerx chan tries: receive LO tuned to channel - 2 MHz first */
        ble_rx((int)num(v[1]), (int)num(v[2]), n > 3 ? (int)num(v[3]) : -1);   /* qs: Q sign */
    } else if (!strcmp(c, "txstream") && n > 2) {     /* txstream units if_khz: phase-continuous tone, generated unit by unit */
        float dph = 2 * (float)M_PI * (float)num(v[2]) * 1000.0f / 40e6f;
        for (uint32_t k = 0; k < 16; k++) txs_tab[k] = rf_iq_word((int)lrintf(300 * cosf(dph * k)), (int)lrintf(300 * sinf(dph * k)));
        rf_tx_on(127, 0);
        rf_dac_stats_t sts;
        uint32_t ps = irq_lock();
        int rc = rf_dac_stream((uint32_t)num(v[1]), txs_cb, 0, &sts);
        irq_unlock(ps);
        rf_tx_off();
        printf("txstream rc %d units %lu late %lu min_margin %lu words idx0 %lu idx1 %lu late_into %lu cb %lu us copy %lu us\n", rc, (unsigned long)sts.units, (unsigned long)sts.late,
               (unsigned long)sts.min_margin, (unsigned long)sts.idx0, (unsigned long)sts.idx1, (unsigned long)sts.late_into, (unsigned long)(sts.cb_max / 240), (unsigned long)(sts.copy_max / 240));
    } else if (!strcmp(c, "phasejump") && n > 2) {    /* phasejump captures f_hz: max phase deviation from linear per 256-sample block */
        int caps = (int)num(v[1]); float f0 = (float)atof(v[2]); float worst = 0; int bad = 0;
        for (int cpt = 0; cpt < caps; cpt++) {
            rf_dump_burst(RF_DUMP_80M, 16383);
            const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
            float ph[63];
            for (int b = 0; b < 63; b++) {
                float re = 0, im = 0;
                for (int k = 0; k < 256; k++) {
                    int idx = b * 256 + k; float a = -2 * (float)M_PI * f0 * idx / 80e6f, cs = cosf(a), sn = sinf(a);
                    float i = rf_iq_i(w[idx]), q = -rf_iq_q(w[idx]);
                    re += i * cs - q * sn; im += i * sn + q * cs;
                }
                ph[b] = atan2f(im, re);
            }
            float sum = 0; for (int b = 1; b < 63; b++) { float d = ph[b] - ph[b - 1]; while (d > (float)M_PI) d -= 2 * (float)M_PI; while (d < -(float)M_PI) d += 2 * (float)M_PI; sum += d; }
            float slope = sum / 62, mx = 0;
            for (int b = 1; b < 63; b++) { float d = ph[b] - ph[b - 1] - slope; while (d > (float)M_PI) d -= 2 * (float)M_PI; while (d < -(float)M_PI) d += 2 * (float)M_PI; if (fabsf(d) > mx) mx = fabsf(d); }
            if (mx > worst) worst = mx;
            if (mx > 0.5f) bad++;
        }
        printf("phasejump %d captures: worst %.3f rad, captures with a jump > 0.5 rad: %d\n", caps, worst, bad);
    } else if (!strcmp(c, "dactest") && n > 1) {     /* dactest mode: 16384-word loop of a +2.5 MHz tone from bank 0 */
        int mode = (int)num(v[1]);
        volatile uint32_t *b0 = (volatile uint32_t *)RF_DUMP_BANK(0);
        for (uint32_t k = 0; k < 16384; k++) { float a = 2 * (float)M_PI * 2.5e6f * k / 40e6f; b0[k] = rf_iq_word((int)lrintf(300 * cosf(a)), (int)lrintf(300 * sinf(a))); }
        rf_tx_on(127, 0);
        volatile uint32_t *bs = (volatile uint32_t *)0x600C101C, *dac = (volatile uint32_t *)0x60033D64, *ct = (volatile uint32_t *)0x60033D5C;
        *ct = 0; *dac = 0;
        *bs = (*bs & ~15u) | 1u | (mode == 2 ? 4u : 0);
        if (mode == 1) { *(volatile uint32_t *)0x60033D90 = 0x000C2040u; *ct = 0x00024000u | RF_DUMP_40M; *bs |= 4; }
        uint32_t d = 1u << 31 | 16384u;
        *dac = d;
        if (mode == 1) *ct = 0x00024000u | RF_DUMP_40M | 1u << 31;
        *dac = d | 1u << 19;
        tx_active = 1;
        if (mode >= 3) {                                 /* 3: bank 1 = bank 0, toggle 0<->1 every ~100 us for 3 s; 4: toggle only on 16-sample boundaries */
            uint32_t c0 = ccount(); volatile uint32_t *b1 = (volatile uint32_t *)RF_DUMP_BANK(1);
            for (uint32_t k = 0; k < 16384; k++) b1[k] = b0[k];
            uint32_t sw = 0; uint64_t cyc = 0; uint32_t cp = c0;
            while (cyc < 720000000ull) {
                uint32_t now = ccount(); cyc += now - cp; cp = now;
                if (cyc / 24000 > sw) {
                    if (mode == 4) { while (1) { now = ccount(); cyc += now - cp; cp = now; if (cyc % 96 < 12) break; } }
                    *bs = (*bs & ~3u) | ((sw & 1) ? 1u : 2u); sw = cyc / 24000;
                }
            }
            *bs = (*bs & ~3u) | 1u;
        }
        printf("dactest %d bank_sel %lx\n", mode, (unsigned long)(*bs & 15));
    } else if (!strcmp(c, "dacidx")) {                /* sample the shared engine index (0x60033D60) while the DAC plays */
        if (n > 1) {                                    /* dacidx 1: also run a circular capture at 40 MS/s into bank 2 (position sensor) */
            volatile uint32_t *bs = (volatile uint32_t *)0x600C101C, *ct = (volatile uint32_t *)0x60033D5C;
            *ct = 0; *(volatile uint32_t *)0x60033D90 = 0x000C2040u; *ct = 0x00024000u | RF_DUMP_40M;
            *bs |= 4; *ct = 0x00024000u | RF_DUMP_40M | 1u << 31;
        }
        uint32_t a[8], t[8];
        for (int k = 0; k < 8; k++) { a[k] = *(volatile uint32_t *)0x60033D60; t[k] = ccount(); delay_us(10); }
        printf("dacidx");
        for (int k = 0; k < 8; k++) printf(" %lu@%lu", (unsigned long)(a[k] & 0x3FFF), (unsigned long)((t[k] - t[0]) / 240));
        printf("\n");
    } else if (!strcmp(c, "dacrewrite") && n > 1) {   /* dacrewrite if_khz: overwrite bank 1 while the DAC keeps playing it */
        volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1); int ik = (int)num(v[1]);
        for (uint32_t k = 0; k < 2000; k++) {
            float ph = 2 * (float)M_PI * (float)ik * 1000.0f * (float)k / 40e6f;
            b[k] = rf_iq_word((int)lrintf(300 * cosf(ph)), (int)lrintf(300 * sinf(ph)));
        }
        printf("dacrewrite %d\n", ik);
    } else if (!strcmp(c, "xtalppb") && n > 2) {      /* xtalppb ppb khz: LO at khz corrected by ppb (+ = crystal fast), cap kept */
        int32_t ppb = (int32_t)num(v[1]); uint64_t hz = (uint64_t)num(v[2]) * 1000u;
        int64_t c = (int64_t)hz - (int64_t)hz * ppb / 1000000000;
        rf_pll_hop_n(rf_pll_n_of_hz((uint64_t)c), rf_pll_cap());
        printf("xtalppb %ld\n", (long)ppb);
    } else if (!strcmp(c, "hopfast") && n > 3) {      /* hopfast khz cap latch(0/1): only frac bytes + cap low (+ latch); time */
        uint32_t khz = (uint32_t)num(v[1]); uint16_t cap = (uint16_t)num(v[2]); int latch = (int)num(v[3]);
        uint32_t nn = (uint32_t)((uint64_t)khz * 4u * 65536u / 120000u - 32u * 65536u);
        uint32_t t0 = ccount();
        if (latch) rf_regi2c_write(0x63, 0x00, 0x07);
        rf_regi2c_write(0x63, 0x04, (uint8_t)(nn >> 8));
        rf_regi2c_write(0x63, 0x05, (uint8_t)nn);
        if (latch) rf_regi2c_write(0x63, 0x00, 0x17);
        rf_regi2c_write(0x62, 0x01, (uint8_t)cap);
        uint32_t t1 = ccount();
        printf("hopfast %.1f us int %lu\n", (t1 - t0) / 240.0, (unsigned long)(nn >> 16));
    } else if (!strcmp(c, "i2cspeed") && n > 1) {     /* i2cspeed v: analog-bus SCL periods (0x6000E054 [15:8] [7:0]); time + verify */
        volatile uint32_t *r54 = (volatile uint32_t *)0x6000E054;
        printf("E050 %08lx E054 %08lx E058 %08lx E05C %08lx E060 %08lx\n", (unsigned long)*(volatile uint32_t *)0x6000E050, (unsigned long)*r54,
               (unsigned long)*(volatile uint32_t *)0x6000E058, (unsigned long)*(volatile uint32_t *)0x6000E05C, (unsigned long)*(volatile uint32_t *)0x6000E060);
        uint32_t val = (uint32_t)num(v[1]);
        *r54 = (*r54 & 0xFFFF0000u) | val << 8 | val;
        uint8_t keep = rf_regi2c_read(0x63, 0x05); int bad = 0;
        uint32_t t0 = ccount();
        for (int k = 0; k < 100; k++) rf_regi2c_write(0x63, 0x05, (uint8_t)k);
        uint32_t t1 = ccount();
        for (int k = 0; k < 50; k++) { rf_regi2c_write(0x63, 0x05, (uint8_t)(k * 37)); if (rf_regi2c_read(0x63, 0x05) != (uint8_t)(k * 37)) bad++; }
        rf_regi2c_write(0x63, 0x05, keep);
        rf_pll_result_t pr; int rc = rf_pll_set_khz(2437000, &pr);
        uint32_t h0 = ccount(); rf_pll_hop(2480000, pr.cap); uint32_t h1 = ccount(); rf_pll_hop(2437000, pr.cap);
        printf("i2cspeed %lu: write %.2f us, readback errors %d/50, pll rc %d locked %d, hop %.1f us\n", (unsigned long)val,
               (ccount(), (t1 - t0) / 240.0 / 100), bad, rc, pr.locked, (h1 - h0) / 240.0);
    } else if (!strcmp(c, "tunehz") && n > 1) {        /* tunehz hz: LO at Hz resolution (both paths) */
        printf("tunehz rc %d\n", rf_tune_hz((uint64_t)strtoull(v[1], 0, 10)));
    } else if (!strcmp(c, "lockscan") && n > 4) {    /* lockscan lo_khz hi_khz step_khz path(0 direct, 1 divider): lock edges and holes */
        uint32_t lo = (uint32_t)num(v[1]), hi = (uint32_t)num(v[2]), stp = (uint32_t)num(v[3]); int path = (int)num(v[4]);
        uint32_t first = 0, last = 0, holes = 0; int prev = -1; uint16_t cap_first = 0, cap_last = 0;
        for (uint32_t f = lo; f <= hi; f += stp) {
            int ok;
            if (path) ok = rf_tune_rx_khz(f) == 0;
            else { rf_pll_result_t r; ok = rf_pll_set_khz(f, &r) == 0 && r.locked; if (ok) { if (!first) cap_first = r.cap; cap_last = r.cap; } }
            if (ok) { if (!first) first = f; else if (prev == 0) holes++; last = f; }
            prev = ok;
        }
        printf("lockscan path %d: first %lu last %lu holes %lu cap %u..%u\n", path, (unsigned long)first, (unsigned long)last, (unsigned long)holes, cap_first, cap_last);
    } else if (!strcmp(c, "hann") && n > 1) {
        hann = (int)num(v[1]);
    } else if (!strcmp(c, "est") && n > 1) {
        int32_t i, q, p; rf_bb_est((uint16_t)num(v[1]), &i, &q, &p); printf("est %ld %ld %ld\n", (long)i, (long)q, (long)p);
    } else if (!strcmp(c, "ard") && n > 2) {
        printf("ard %02x\n", rf_regi2c_read((uint8_t)num(v[1]), (uint8_t)num(v[2])));
    } else if (!strcmp(c, "awr") && n > 3) {
        rf_regi2c_write((uint8_t)num(v[1]), (uint8_t)num(v[2]), (uint8_t)num(v[3]));
    } else if (!strcmp(c, "rd") && n > 1) {
        printf("rd %08lx\n", (unsigned long)R((uint32_t)strtoul(v[1], 0, 0)));
    } else if (!strcmp(c, "wr") && n > 2) {
        R((uint32_t)strtoul(v[1], 0, 0)) = (uint32_t)strtoul(v[2], 0, 0);
    } else if (!strcmp(c, "delay") && n > 1) {
        delay_us((uint32_t)num(v[1]));
    } else if (!strcmp(c, "tempraw") && n > 1) {      /* tempraw dac: raw sensor code with range DAC = dac */
        static int ts2; if (!ts2) { tsens_init(); ts2 = 1; }
        regi2c_write_mask(0x69, 0x06, 3, 0, (uint8_t)num(v[1])); delay_us(500);
        R(0x60008850) |= 1u << 24; while (!(R(0x60008850) & 1u << 8)) {}
        uint32_t raw = R(0x60008850) & 0xFF; R(0x60008850) &= ~(1u << 24);
        printf("tempraw dac %ld raw %lu 69.06 %02x\n", num(v[1]), (unsigned long)raw, regi2c_read(0x69, 0x06));
    } else if (!strcmp(c, "temp")) {
        static int ts; if (!ts) { tsens_init(); ts = 1; }
        printf("temp %.1f\n", tsens_read_c());
    } else { printf("err unknown %s\n", c); return; }
    printf("ok\n");
}

int main(void)
{
    delay_ms(300);
    printf("rfctl ready\n");
    int k = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0) continue;
        if (ch == '\r') continue;
        if (ch == '\n') { line[k] = 0; command(); k = 0; continue; }
        if (k < (int)sizeof line - 1) line[k++] = (char)ch;
    }
}
