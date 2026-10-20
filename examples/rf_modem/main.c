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
    char *v[10]; int n = 0;
    for (char *p = line; *p && n < 10; ) {
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
    } else if (!strcmp(v[0], "rawtx") && n > 5) {         /* rawtx ch_idx rate sync type len: raw packets, then count for 1 s */
        static uint8_t d[255]; for (int k = 0; k < 255; k++) d[k] = (uint8_t)k;
        if (n > 6) {                                       /* 7th arg: prefix mode 1 = preamble 0x55 + sync (LSB first) at the buffer start, 2 = 0xAA + sync */
            uint32_t sw = (uint32_t)strtoul(v[3], 0, 0); int m = atoi(v[6]);
            d[0] = m == 2 ? 0xAA : 0x55; d[1] = (uint8_t)sw; d[2] = (uint8_t)(sw >> 8); d[3] = (uint8_t)(sw >> 16); d[4] = (uint8_t)(sw >> 24);
            for (int k = 5; k < 255; k++) d[k] = (uint8_t)(k - 5);
        }
        extern uint32_t ble_hw_raw_poke_off, ble_hw_raw_poke_or;   /* 8th arg "off:bits": OR into a core register before the kick */
        ble_hw_raw_poke_off = 0;
        if (n > 7) { char *c = v[7]; ble_hw_raw_poke_off = (uint32_t)strtoul(c, &c, 0); ble_hw_raw_poke_or = (uint32_t)strtoul(c + 1, 0, 0); }
        int rc = ble_hw_raw_tx((uint8_t)atoi(v[1]), (uint8_t)atoi(v[2]), (uint32_t)strtoul(v[3], 0, 0), 0x555555, (uint8_t)atoi(v[4]), d, (unsigned)atoi(v[5]), 8);
        uint32_t c0 = ble_hw_raw_txcount(); delay_ms(1000);
        printf("rawtx rc %d packets/s %lu core00 %08lx st %08lx\n", rc, (unsigned long)(ble_hw_raw_txcount() - c0), (unsigned long)*(volatile uint32_t *)0x60031000, (unsigned long)*(volatile uint32_t *)0x60031014);
    } else if (!strcmp(v[0], "emr") && n > 1) {           /* emr off [count]: exchange-memory words */
        uint32_t o = (uint32_t)strtoul(v[1], 0, 0), c = n > 2 ? (uint32_t)atoi(v[2]) : 1;
        printf("emr %04lx:", (unsigned long)o); for (uint32_t k = 0; k < c; k++) printf(" %04x", ble_hw_em_rd(o + 2 * k)); printf("\n");
    } else if (!strcmp(v[0], "emw") && n > 2) {
        ble_hw_em_wr((uint32_t)strtoul(v[1], 0, 0), (uint16_t)strtoul(v[2], 0, 0)); printf("emw\n");
    } else if (!strcmp(v[0], "rawrx") && n > 4) {         /* rawrx ch_idx rate sync ms: receive, count, show the first packets */
        extern uint16_t ble_hw_rx_set_off, ble_hw_rx_set_val; ble_hw_rx_set_off = 0;
        if (n > 5) { char *c = v[5]; ble_hw_rx_set_off = (uint16_t)strtoul(c, &c, 0); uint32_t vv = (uint32_t)strtoul(c + 1, 0, 0); ble_hw_rx_set_val = (uint16_t)vv; extern uint32_t ble_hw_rx_set_val32; ble_hw_rx_set_val32 = vv; }   /* 6th arg "csoff:val" (off >= 0x1000: core reg off-0x1000) */
        extern uint8_t ble_hw_rx_libphy, ble_hw_rx_big; ble_hw_rx_libphy = n > 6 ? (uint8_t)(atoi(v[6]) & 1) : 0;   /* 7th arg: bit 0 libphy core values, bit 1 big buffers */
        ble_hw_rx_big = n > 6 ? (uint8_t)((atoi(v[6]) >> 1) & 1) : 0;
        int rc = ble_hw_raw_rx((uint8_t)atoi(v[1]), (uint8_t)atoi(v[2]), (uint32_t)strtoul(v[3], 0, 0), 0x555555);
        uint32_t t0 = ccount(), dur = (uint32_t)atoi(v[4]) * 240000u, got = 0, bad = 0; uint8_t b[64]; unsigned len; uint16_t st; int8_t rs;
        uint32_t c0 = ble_hw_raw_rxcount();
        while (ccount() - t0 < dur) {
            if (!ble_hw_raw_read(b, sizeof b, &len, &st, &rs)) continue;
            if (st & 0x0F) bad++; else got++;
            if (got + bad <= 3) { printf("pkt len %u st %04x rssi %d:", len, st, rs); for (unsigned k = 0; k < (len < 12 ? len : 12); k++) printf(" %02x", b[k]);
                if (len > 12) { printf(" .."); for (unsigned k = (len < 64 ? len : 64) - 4; k < (len < 64 ? len : 64); k++) printf(" %02x", b[k]); } printf("\n"); }
        }
        printf("rawrx rc %d ok %lu bad %lu core374 %lu core00 %08lx st %08lx\n", rc, (unsigned long)got, (unsigned long)bad, (unsigned long)(ble_hw_raw_rxcount() - c0),
               (unsigned long)*(volatile uint32_t *)0x60031000, (unsigned long)*(volatile uint32_t *)0x60031014);
    } else if (!strcmp(v[0], "rawsend") && n > 4) {       /* rawsend ch_idx rate sync len: our bytes 00 01 02 ... (header byte 0x5A), one packet per event */
        static uint8_t d[255]; for (int k = 0; k < 255; k++) d[k] = (uint8_t)k;
        extern uint8_t ble_hw_raw_fmt, ble_hw_raw_tmpl; extern uint16_t ble_hw_raw_t18;
        if (n > 5) ble_hw_raw_fmt = (uint8_t)atoi(v[5]);
        ble_hw_raw_tmpl = n > 6 ? (uint8_t)atoi(v[6]) : 0; ble_hw_raw_t18 = n > 7 ? (uint16_t)strtoul(v[7], 0, 0) : 0xFFFF;
        int rc = ble_hw_raw_send((uint8_t)atoi(v[1]), (uint8_t)atoi(v[2]), (uint32_t)strtoul(v[3], 0, 0), 0x555555, 0x5A, d, (unsigned)atoi(v[4]));
        ble_hw_stats_t s0, s1; ble_hw_get_stats(&s0); delay_ms(1000); ble_hw_get_stats(&s1);
        printf("rawsend rc %d events/s %lu\n", rc, (unsigned long)(s1.events - s0.events));
    } else if (!strcmp(v[0], "rawstop")) {
        ble_hw_raw_stop(); printf("rawstop\n");
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
