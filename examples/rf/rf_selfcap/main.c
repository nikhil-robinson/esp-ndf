/* rf_selfcap: receive while transmitting on one board. selfcap <mode> <offset_khz> [gain_db]
 *   mode 0: DAC tone (rf_tx_tone_khz), mode 1: hardware tone generator (0x60006040 bit 18, DAC path off), mode 2: continuous packets (rf_pkt_send), mode 3: mode 1 plus the RX front end forced on through the pbus (rf_pbus_xpd_rx_on)
 * During the transmission the receive front end is forced on (rf_rx_on) and the dump engine captures 8000 words at 16 MS/s.
 * Prints: words that step by one (the shared DAC/capture address counter), mean power, the power at +offset vs the median
 * of 16 other bins (dB), and the same tone check on a capture after the transmitter is off (the receive path still works). */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include "rf.h"
#include "rf_init.h"
#include "rf_dump.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define N 8000
static float bin_db(const uint32_t *iq, float f_hz)         /* |DFT| at f (16 MS/s), dB */
{
    float re = 0, im = 0, c = cosf(-2 * (float)M_PI * f_hz / 16e6f), s = sinf(-2 * (float)M_PI * f_hz / 16e6f), pr = 1, pi = 0;
    for (int k = 0; k < N; k++) {
        float i = (float)rf_sample_i(iq[k]), q = (float)rf_sample_q(iq[k]);
        re += i * pr - q * pi; im += i * pi + q * pr;
        float t = pr * c - pi * s; pi = pr * s + pi * c; pr = t;
    }
    return 10 * log10f((re * re + im * im) / N + 1e-9f);
}
static void report(const char *tag, const uint32_t *iq, int off_khz)
{
    unsigned step1 = 0; double pw = 0;
    for (int k = 0; k < N; k++) { if (k && iq[k] == iq[k - 1] + 1) step1++; int i = rf_sample_i(iq[k]), q = rf_sample_q(iq[k]); pw += i * i + q * q; }
    float o[16]; for (int j = 0; j < 16; j++) o[j] = bin_db(iq, (float)(off_khz + 300 + 211 * j) * 1000.0f);
    for (int a = 0; a < 16; a++) for (int b = a + 1; b < 16; b++) if (o[b] < o[a]) { float t = o[a]; o[a] = o[b]; o[b] = t; }
    float t = bin_db(iq, (float)off_khz * 1000.0f);
    int imin = 512, imax = -512; unsigned sat = 0; static uint8_t seen[1024]; memset(seen, 0, sizeof seen); unsigned distinct = 0;
    for (int k = 0; k < N; k++) { int i = rf_sample_i(iq[k]); if (i < imin) imin = i; if (i > imax) imax = i; if (i >= 500 || i <= -500) sat++; if (!seen[i + 512]++) distinct++; }
    printf(" %s counter %u pwr %.1f tone %.1f dB over %.1f (snr %.1f) I %d..%d distinct %u sat %u w %05lx %05lx", tag, step1, pw / N, (double)t, (double)o[8], (double)(t - o[8]),
           imin, imax, distinct, sat, (unsigned long)(iq[100] & 0xFFFFF), (unsigned long)(iq[3001] & 0xFFFFF));
}

static void tone_hw(int on)
{
    if (on) { *(volatile uint32_t *)0x60006000u &= ~(1u << 26); *(volatile uint32_t *)0x60006040u |= 1u << 18; }
    else rf_tone_off();
}

static char line[96];
static void command(void)
{
    char *v[8]; int n = 0;
    for (char *p = line; *p && n < 8; ) { while (*p == ' ') *p++ = 0; if (!*p) break; v[n++] = p; while (*p && *p != ' ') p++; }
    if (!n) return;
    if (!strcmp(v[0], "open")) printf("open rc %d\n", rf_open());
    else if (!strcmp(v[0], "tune") && n > 1) {          /* tune khz: our synthesizer, then the PLL block 0x62 and 0x63 registers 0..11 */
        extern uint8_t rf_regi2c_read(uint8_t, uint8_t);
        int rc = rf_set_freq_khz((uint32_t)atoi(v[1])); printf("tune %s rc %d", v[1], rc);
        for (int b = 0x62; b <= 0x63; b++) { printf(" %02x:", b); for (int r = 0; r < 12; r++) printf(" %02x", rf_regi2c_read((uint8_t)b, (uint8_t)r)); }
        printf("\n");
    }
    else if (!strcmp(v[0], "selfcap") && n > 2) {
        int mode = atoi(v[1]), off = atoi(v[2]), g = n > 3 ? atoi(v[3]) : 10, rc = 0;
        rf_pkt_stop(); rf_tx_stop(); rf_set_freq_khz(2440000);
        static const uint8_t pl[37] = { 1, 2, 3 };
        rf_pkt_cfg_t pc = { .mhz = 2440, .rate = RF_PKT_1M, .sync = 0x71764129, .crc_init = 0x555555 };
        if (mode == 0) { rc = rf_tx_start(); if (!rc) rc = rf_tx_tone_khz(off, 200); }
        else if (mode == 1) { rc = rf_tx_start(); rf_dac_stop(); tone_hw(1); }
        else if (mode == 2) rc = rf_pkt_send(&pc, 0, pl, sizeof pl, 0);
        else { rc = rf_tx_start(); rf_dac_stop(); tone_hw(1); }
        delay_us(500);
        rf_rx_on(); rf_rx_gain(g < 0 ? -1 : g);
        if (mode == 3) rf_pbus_xpd_rx_on((uint16_t)(g < 0 ? 0 : g));   /* front-end override on top of the TX state */
        delay_us(200);
        const uint32_t *cap = (const uint32_t *)RF_DUMP_BURST_ADDR;     /* each capture is analysed in place (RAM must stay below the dump banks) */
        int c1 = rf_dump_burst(RF_DUMP_16M, N);
        printf("selfcap mode %d off %d gain %d rc %d cap1 %d", mode, off, g, rc, c1); report("tx1", cap, off);
        int c2 = rf_dump_burst(RF_DUMP_16M, N);                 /* second capture still during TX: did the first one stop the transmitter? */
        printf(" cap2 %d", c2); report("tx2", cap, off);
        if (mode == 1 || mode == 3) tone_hw(0);
        rf_pkt_stop(); rf_tx_stop();
        rf_rx_start(); rf_rx_gain(g < 0 ? -1 : g); delay_us(200);
        const uint32_t *iq; int c3 = rf_rx_capture(RF_RATE_16M, N, &iq);
        printf(" cap3 %d", c3); report("after", iq, off); printf("\n");
    } else { printf("err unknown\n"); return; }
}

int main(void)
{
    delay_ms(300);
    printf("rfctl ready (rf_selfcap)\n");
    int k = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0) continue;
        if (ch == '\r' || ch == '\n') { line[k] = 0; if (k) { command(); printf("ok\n"); } k = 0; }
        else if (k < (int)sizeof line - 1) line[k++] = (char)ch;
    }
}
