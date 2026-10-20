/* Bench: map the BT modem block (0x60011000) below the BLE link layer. UART commands:
 *   init                 our PHY init + the BLE hardware setup (modem, coex, sequencer, frequency memory), core never kicked
 *   rx khz               receive front end on at khz (no BLE event)
 *   adv chan interval_us hardware advertising (the transmitting board)
 *   watch ms             read all 512 modem words for ms; print every word that changed (count, min, max, last) */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include "ble_hw.h"
#include <stdlib.h>
#include <string.h>
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"

static uint32_t wbase = 0x60011000u, wn = 64;
#define MOD(k) (*(volatile uint32_t *)(wbase + 4u * (k)))
static char line[96];
static uint32_t base[512], lo[512], hi[512], last[512];
static uint16_t cnt[512];

static void command(void)
{
    char *v[6]; int n = 0;
    for (char *p = line; *p && n < 6; ) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        v[n++] = p; while (*p && *p != ' ') p++;
    }
    if (!n) return;
    if (!strcmp(v[0], "init")) {
        rf_init_replay();
        rf_pll_result_t r; rf_pll_set_khz(2402000, &r);
        printf("init ble_hw_init %d\n", ble_hw_init());
    } else if (!strcmp(v[0], "rx") && n > 1) {
        rf_rx_on(); printf("rx tune %d\n", rf_tune_rx_khz((uint32_t)atoi(v[1])));
    } else if (!strcmp(v[0], "adv") && n > 2) {
        static const uint8_t a[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 }; uint8_t d[31] = { 0 };
        printf("adv %d\n", ble_hw_adv((uint8_t)atoi(v[1]), a, 1, d, 31, (uint32_t)atoi(v[2])));
    } else if (!strcmp(v[0], "wr") && n > 2) {
        *(volatile uint32_t *)strtoul(v[1], 0, 0) = (uint32_t)strtoul(v[2], 0, 0); printf("wr\n");
    } else if (!strcmp(v[0], "rd") && n > 1) {
        printf("rd %08lx\n", (unsigned long)*(volatile uint32_t *)strtoul(v[1], 0, 0));
    } else if (!strcmp(v[0], "bitscan") && n > 2) {      /* bitscan base words: flip each bit (core idle); hit = modem status starts moving */
        uint32_t b0 = (uint32_t)strtoul(v[1], 0, 0), nw = (uint32_t)atoi(v[2]); int hits = 0;
        static const uint8_t st[5] = { 0x6C / 4, 0x70 / 4, 0x7C / 4, 0x80 / 4, 0xB0 / 4 };
        for (uint32_t k = 0; k < nw; k++) {
            volatile uint32_t *r = (volatile uint32_t *)(b0 + 4 * k);
            if (b0 == 0x60011000u && (k == st[0] || k == st[1] || k == st[2] || k == st[3] || k == st[4])) continue;
            uint32_t save = *r;
            printf("w %lx\n", (unsigned long)(b0 + 4 * k));
            for (int b = 0; b < 32; b++) {
                *r = save ^ (1u << b);
                uint32_t s0[5], ch = 0, t0 = ccount();
                for (int j = 0; j < 5; j++) s0[j] = (*(volatile uint32_t *)(0x60011000u + 4u * st[j]));
                while (ccount() - t0 < 240000u * 10) for (int j = 0; j < 5; j++) if ((*(volatile uint32_t *)(0x60011000u + 4u * st[j])) != s0[j]) ch |= 1u << j;
                *r = save;
                if (ch) { printf("hit %08lx bit %d status mask %lx\n", (unsigned long)(b0 + 4 * k), b, (unsigned long)ch); hits++; }
            }
        }
        printf("bitscan hits %d\n", hits);
    } else if (!strcmp(v[0], "scan") && n > 1) {
        ble_hw_scan((uint8_t)atoi(v[1])); printf("scan\n");
    } else if (!strcmp(v[0], "stop")) {
        ble_hw_stop(); printf("stop\n");
    } else if (!strcmp(v[0], "pkts")) {
        ble_hw_pkt_t p; int c = 0; while (ble_hw_read(&p)) c++; printf("pkts %d\n", c);
    } else if (!strcmp(v[0], "pwr") && n > 1) {          /* pwr ms: max / mean baseband power estimate */
        int32_t i, q, pw, mx = 0; int64_t sum = 0; uint32_t c = 0, t0 = ccount(), dur = (uint32_t)atoi(v[1]) * 240000u;
        while (ccount() - t0 < dur) { rf_bb_est(63, &i, &q, &pw); if (pw > mx) mx = pw; sum += pw; c++; }
        printf("pwr max %ld mean %ld n %lu\n", (long)mx, (long)(sum / (c ? c : 1)), (unsigned long)c);
    } else if (!strcmp(v[0], "watch") && n > 1) {          /* watch ms [base words]: default the modem block (0x100, mirrored x8) */
        wbase = n > 2 ? (uint32_t)strtoul(v[2], 0, 0) : 0x60011000u; wn = n > 3 ? (uint32_t)atoi(v[3]) : 64; if (wn > 512) wn = 512;
        for (int k = 0; k < (int)wn; k++) { base[k] = lo[k] = hi[k] = last[k] = MOD(k); cnt[k] = 0; }
        uint32_t t0 = ccount(), dur = (uint32_t)atoi(v[1]) * 240000u, reads = 0;
        while (ccount() - t0 < dur) {
            for (int k = 0; k < (int)wn; k++) {
                uint32_t x = MOD(k);
                if (x != last[k]) { if (cnt[k] < 65535) cnt[k]++; last[k] = x; }
                if (x < lo[k]) lo[k] = x;
                if (x > hi[k]) hi[k] = x;
            }
            reads++;
        }
        printf("watch %lu passes\n", (unsigned long)reads);
        for (int k = 0; k < (int)wn; k++) if (cnt[k] || hi[k] != lo[k])
            printf("  %08lx base %08lx n %u lo %08lx hi %08lx last %08lx\n", (unsigned long)(wbase + 4 * k), (unsigned long)base[k], cnt[k], (unsigned long)lo[k], (unsigned long)hi[k], (unsigned long)last[k]);
        printf("watch end\n");
    }
}

int main(void)
{
    delay_ms(300);
    printf("rfctl ready (rf_modem)\n");
    int k = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch == '\n') { line[k] = 0; command(); printf("ok\n"); k = 0; continue; }
        if (k < (int)sizeof line - 1) line[k++] = (char)ch;
    }
}
