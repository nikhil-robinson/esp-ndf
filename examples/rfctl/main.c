/* rfctl: the radio driven by line commands over the console, for host-scripted
 * measurements (tools/rfhost.py). Every command answers with result lines and
 * then "ok" (or "err ..."). Frequencies in Hz are in the true spectrum. */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../drivers/rf/rf_regi2c.h"
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"
#include "../../drivers/rf/rf_dump.h"

#define R(a) (*(volatile uint32_t *)(a))

static float bin_db(const uint32_t *w, uint32_t n, float f, float fs)
{
    float c = cosf(2 * (float)M_PI * f / fs), s = sinf(2 * (float)M_PI * f / fs), re = 0, im = 0, cr = 1, ci = 0;
    for (uint32_t k = 0; k < n; k++) {
        float i = rf_iq_i(w[k]), q = -rf_iq_q(w[k]);
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
        delay_us(300);
        uint32_t t = ccount();
        rf_pll_hop((uint32_t)num(v[3]), (uint16_t)num(v[4]));
        uint32_t hop = ccount() - t;
        rf_dump_burst(RF_DUMP_80M, 8192);
        const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
        printf("hopmeas %lu cycles:", (unsigned long)hop);
        for (int k = 0; k < 32; k++) printf(" %.1f", bin_db(w + 256 * k, 256, (float)atof(v[5]), 80e6f));
        printf("\n");
    } else if (!strcmp(c, "pbus") && n > 3) {
        rf_pbus_write((uint8_t)num(v[1]), (uint8_t)num(v[2]), (uint16_t)num(v[3]));
    } else if (!strcmp(c, "pmem") && n > 2) {         /* pmem idx value: one pbus-memory word (address 0x200 + idx) */
        R(0x600060CC) = (uint32_t)strtoul(v[2], 0, 0);
        R(0x600060C8) = (R(0x600060C8) & 0xFFFC00FFu) | (uint32_t)((0x200 + num(v[1])) & 0x3FF) << 8;
        R(0x600060C8) &= 0xFFFCFFFFu;
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
    } else if (!strcmp(c, "temp")) {
        printf("temp n/a\n");
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
