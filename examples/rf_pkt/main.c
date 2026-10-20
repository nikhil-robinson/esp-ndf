/* rf_pkt: the hardware packet engine (rf.h) driven by line commands, for tools/rf_pkt_test.py (two boards).
 *   send mhz rate sync flags len count     payload 00 01 02 ...; count 0 = continuous; prints packets/s over 1 s
 *   rx mhz rate sync flags gain ms         receive for ms: good / CRC-bad packets, wrong bytes, RSSI, CPU
 *   stop | limits min_khz max_khz */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include "rf.h"
#include "ble_hw.h"
#include <stdlib.h>
#include <string.h>

static char line[96];
static uint8_t payload[255];

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
        rf_pkt_cfg_t c = { (uint16_t)atoi(v[1]), (uint8_t)atoi(v[2]), (uint8_t)atoi(v[4]), (uint32_t)strtoul(v[3], 0, 0), 0x555555 };
        uint32_t cnt = (uint32_t)strtoul(v[6], 0, 0);
        ble_hw_stats_t s0, s1; ble_hw_get_stats(&s0); uint32_t t0 = ccount();
        int rc = rf_pkt_send(&c, 0x5A, payload, (unsigned)atoi(v[5]), cnt);
        if (rc) { printf("send rc %d\n", rc); return; }
        if (cnt) { while (rf_pkt_busy() && ccount() - t0 < 240000000u * 10) {} ble_hw_get_stats(&s1);
                   printf("send rc 0 sent %lu in %lu ms\n", (unsigned long)(s1.events - s0.events), (unsigned long)((ccount() - t0) / 240000)); return; }
        delay_ms(1000); ble_hw_get_stats(&s1);
        printf("send rc 0 packets/s %lu cpu %.3f%% ctl %08lx\n", (unsigned long)(s1.events - s0.events), (double)(s1.isr_cycles - s0.isr_cycles) / 2.4e6, (unsigned long)*(volatile uint32_t *)0x60031000);
    } else if (!strcmp(v[0], "rx") && n > 6) {
        rf_pkt_cfg_t c = { (uint16_t)atoi(v[1]), (uint8_t)atoi(v[2]), (uint8_t)atoi(v[4]), (uint32_t)strtoul(v[3], 0, 0), 0x555555 };
        int rc = rf_pkt_rx_start(&c, atoi(v[5]));
        if (rc) { printf("rx rc %d\n", rc); return; }
        static rf_pkt_t p; uint32_t good = 0, crc = 0, wrong = 0, lens = 0, t0 = ccount(), dur = (uint32_t)atoi(v[6]) * 240000u; int32_t rs = 0; uint8_t hdr = 0; uint32_t crcdata = 0;
        while (ccount() - t0 < dur) {
            if (!rf_pkt_read(&p)) continue;
            if (p.status & 1) continue;                /* window ended without a packet */
            if (!p.ok) { crc++; unsigned m = 0; while (m < p.len && p.data[m] == (uint8_t)m) m++; if (p.len && m == p.len) crcdata++; continue; }
            good++; rs += p.rssi; lens = p.len; hdr = p.hdr;
            for (unsigned k = 0; k < p.len; k++) if (p.data[k] != (uint8_t)k) { wrong++; break; }
        }
        uint32_t ctl = *(volatile uint32_t *)0x60031000;
        rf_pkt_stop();
        printf("rx ctl %08lx\n", (unsigned long)ctl);
        printf("rx rc 0 good %lu crcbad %lu crcbad_dataok %lu wrong %lu len %lu hdr %02x rssi %ld\n", (unsigned long)good, (unsigned long)crc, (unsigned long)crcdata, (unsigned long)wrong, (unsigned long)lens, hdr, (long)(good ? rs / (int32_t)good : 0));
    } else if (!strcmp(v[0], "rxpoke") && n > 2) {          /* bench: set one RX CS field (offset) on the next rx */
        extern uint16_t ble_hw_rx_set_off, ble_hw_rx_set_val; ble_hw_rx_set_off = (uint16_t)strtoul(v[1], 0, 0); ble_hw_rx_set_val = (uint16_t)strtoul(v[2], 0, 0); printf("rxpoke\n");
    } else if (!strcmp(v[0], "core") && n > 2) {            /* bench: OR / AND-NOT into RWBLECNTL: core or|andnot */
        *(volatile uint32_t *)0x60031000 = (*(volatile uint32_t *)0x60031000 | (uint32_t)strtoul(v[1], 0, 0)) & ~(uint32_t)strtoul(v[2], 0, 0); printf("core %08lx\n", (unsigned long)*(volatile uint32_t *)0x60031000);
    } else if (!strcmp(v[0], "txflip") && n > 2) {          /* bench: XOR bits into one send CS field */
        extern uint16_t ble_hw_tx_flip_off, ble_hw_tx_flip_bits; ble_hw_tx_flip_off = (uint16_t)strtoul(v[1], 0, 0); ble_hw_tx_flip_bits = (uint16_t)strtoul(v[2], 0, 0); printf("txflip\n");
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
