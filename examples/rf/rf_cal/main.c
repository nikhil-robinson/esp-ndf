/* rf_cal: the calibration store, driven by line commands (tools/rf/rf_cal_test.py).
 *   open | close | erase | tone khz offset_khz | dcres (RX DC residual at gain indices 0/30/60/90/127, terminated input not needed: mean of 4096 samples) */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include "rf.h"
#include <stdlib.h>
#include <string.h>

static char line[96];
static void command(void)
{
    char *v[8]; int n = 0;
    for (char *p = line; *p && n < 8; ) { while (*p == ' ') *p++ = 0; if (!*p) break; v[n++] = p; while (*p && *p != ' ') p++; }
    if (!n) return;
    if (!strcmp(v[0], "open")) {
        uint32_t t0 = ccount(); int rc = rf_open(); uint32_t t1 = ccount();
        printf("open rc %d source %d in %lu ms temp %.1f cal_temp %.1f\n", rc, rf_cal_source(), (unsigned long)((t1 - t0) / 240000), (double)rf_temp_c(), (double)rf_cal_temp_c());
    } else if (!strcmp(v[0], "close")) { rf_close(); printf("close\n");
    } else if (!strcmp(v[0], "erase")) { printf("erase rc %d\n", rf_cal_erase());
    } else if (!strcmp(v[0], "tone") && n > 2) {
        rf_tx_stop(); int rc = rf_set_freq_khz((uint32_t)atoi(v[1]));
        uint32_t t0 = ccount(); if (!rc) rc = rf_tx_start(); uint32_t t1 = ccount();
        if (!rc) rc = rf_tx_tone_khz(atoi(v[2]), 300);
        printf("tone rc %d tx_start %lu ms\n", rc, (unsigned long)((t1 - t0) / 240000));
    } else if (!strcmp(v[0], "dcres")) {
        rf_tx_stop(); rf_rx_start(); printf("dcres");
        static const int g[] = { 0, 30, 60, 90, 127 };
        for (int k = 0; k < 5; k++) {
            rf_rx_set_gain(g[k]); delay_us(200);
            const uint32_t *iq; rf_rx_capture(RF_RATE_40M, 4096, &iq); int32_t si = 0, sq = 0;
            for (int j = 0; j < 4096; j++) { si += rf_sample_i(iq[j]); sq += rf_sample_q(iq[j]); }
            printf(" g%d:%.1f/%.1f", g[k], si / 4096.0, sq / 4096.0);
        }
        rf_rx_set_gain(RF_GAIN_AGC); printf("\n");
    } else if (!strcmp(v[0], "guard") && n > 1) {        /* guard ms: tx_max_ms */
        rf_limits_t l; rf_get_limits(&l); l.tx_max_ms = (uint32_t)atoi(v[1]); rf_set_limits(&l); printf("guard\n");
    } else if (!strcmp(v[0], "state")) {
        printf("state %d expired %d\n", rf_state(), rf_tx_expired());
    } else if (!strcmp(v[0], "cycles") && n > 1) {       /* cycles n: open / tx tone / hop / close, n times; failures counted */
        int bad = 0; uint32_t t0 = ccount();
        for (int k = 0; k < atoi(v[1]); k++) {
            if (rf_open()) bad++;
            if (rf_set_freq_khz(2402000 + 2000u * (uint32_t)(k % 40))) bad++;
            if (rf_tx_start() || rf_tx_tone_khz(1000, 200)) bad++;
            if (rf_tx_stop()) bad++;
            if (!rf_pll_locked()) bad++;
            rf_close();
        }
        printf("cycles %d failures %d in %lu ms\n", atoi(v[1]), bad, (unsigned long)((ccount() - t0) / 240000));
    } else { printf("err unknown\n"); return; }
}

int main(void)
{
    delay_ms(300);
    printf("rfctl ready (rf_cal)\n");
    int k = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch == '\n') { line[k] = 0; command(); printf("ok\n"); k = 0; continue; }
        if (k < (int)sizeof line - 1) line[k++] = (char)ch;
    }
}
