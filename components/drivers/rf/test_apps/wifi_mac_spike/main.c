/* Wi-Fi MAC spike: what the MAC does without the vendor stack (docs/WIFI_MAC.md). Line commands over the console:
 *   rxinit                  carrier on channel 6 + RX front end + our ring
 *   mac aa:bb:cc:dd:ee:ff   address filter slot 0 + ACK enable (same offsets as the ESP32: filter 0x60033040, ACK en 0x60033064 bit 16)
 *   rx <ms> [v]             receive; per frame: type, length, header words; summary by 802.11 type
 *   rd <addr> [n]           read n words      wr <addr> <val>   write a word
 *   txinit / tx <hex>       carrier + TX front end; send one raw frame (rf_wifi_tx) */
#include "console.h"
#include "cpu.h"
#include "sys.h"
#include "rf.h"
#include <stdlib.h>
#include <string.h>

static char line[300];
static int hexbin(const char *h, uint8_t *o, int max) { int n = 0; for (; h[0] && h[1] && n < max; h += 2, n++) { char b[3] = { h[0], h[1], 0 }; o[n] = (uint8_t)strtoul(b, 0, 16); } return n; }
static uint8_t my[6];
static int parse_mac(const char *s, unsigned m[6]) { for (int i = 0; i < 6; i++) { char *e; m[i] = strtoul(s, &e, 16); if (e == s) return -1; s = e + (*e == ':'); } return 0; }
#define R32(a) (*(volatile uint32_t *)(a))

int main(void)
{
    delay_ms(300);
    printf("wifi_mac_spike ready open %d\n", rf_open());
    int k = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch != '\n') { if (k < (int)sizeof line - 1) line[k++] = (char)ch; continue; }
        line[k] = 0; k = 0;
        char *v[6]; int n = 0;
        for (char *p = line; *p && n < 6; ) { while (*p == ' ') *p++ = 0; if (!*p) break; v[n++] = p; while (*p && *p != ' ') p++; }
        if (!n) { printf("ok\n"); continue; }
        if (!strcmp(v[0], "rxinit")) printf("rxinit %d\n", rf_wifi_rx_init());
        else if (!strcmp(v[0], "time")) {
            for (int i = 0; i < 3; i++) {
                uint32_t a = ccount(); rf_wifi_tx_init(); uint32_t b = ccount(); rf_wifi_rx_init(); uint32_t c = ccount();
                printf("tx_init %lu us, rx_init %lu us\n", (unsigned long)((b - a) / 240), (unsigned long)((c - b) / 240));
            }
        } else if (!strcmp(v[0], "auth") && n > 2) {          /* auth <bssid> <mymac>: open-system authentication request, then listen */
            unsigned bs[6], me[6]; if (parse_mac(v[1], bs) || parse_mac(v[2], me)) { printf("bad mac\n"); continue; }
            uint8_t f[36] = { 0xB0, 0x00, 0x3A, 0x01 };
            for (int i = 0; i < 6; i++) { f[4 + i] = (uint8_t)bs[i]; f[10 + i] = (uint8_t)me[i]; f[16 + i] = (uint8_t)bs[i]; my[i] = (uint8_t)me[i]; }
            f[22] = 0x10; f[23] = 0x00; f[24] = 0; f[25] = 0; f[26] = 1; f[27] = 0; f[28] = 0; f[29] = 0;
            unsigned flen = 30; uint8_t bb[6]; for (int i = 0; i < 6; i++) bb[i] = (uint8_t)bs[i];
            if (n > 3 && !strcmp(v[3], "probe")) {                  /* broadcast probe request: the AP answers it with a unicast probe response and needs no state */
                static const uint8_t ie[8] = { 0, 0, 1, 4, 0x82, 0x84, 0x8b, 0x96 };
                f[0] = 0x40; f[1] = 0; for (int i = 0; i < 6; i++) { f[4 + i] = 0xFF; f[16 + i] = 0xFF; }
                memcpy(f + 24, ie, 8); flen = 32;
            }
            if (n > 4 && !strcmp(v[4], "fcs")) {                    /* append the frame check sequence ourselves: IEEE CRC-32, little endian */
                uint32_t c = 0xFFFFFFFFu;
                for (unsigned i = 0; i < flen; i++) { c ^= f[i]; for (int k = 0; k < 8; k++) c = c & 1 ? c >> 1 ^ 0xEDB88320u : c >> 1; }
                c = ~c; for (int i = 0; i < 4; i++) f[flen + i] = (uint8_t)(c >> 8 * i); flen += 4;
            }
            R32(0x60033040) = me[0] | me[1] << 8 | me[2] << 16 | me[3] << 24; R32(0x60033044) = me[4] | me[5] << 8;
            for (int attempt = 0; attempt < 3; attempt++) {
                rf_wifi_tx_init(); delay_us(1000); int rc = rf_wifi_tx(f, flen);          /* settle time after the TX front end comes up (measured below) */
                delay_us(192 + 8 * (flen + 4) + 100);                  /* the done flag comes before the frame has left the air: 1 Mbit/s, long preamble */
                uint32_t t0 = ccount(); rf_wifi_rx_init(); uint32_t sw = (ccount() - t0) / 240;
                R32(0x60033040) = me[0] | me[1] << 8 | me[2] << 16 | me[3] << 24; R32(0x60033044) = me[4] | me[5] << 8;
                static uint8_t b[1700]; uint32_t got = 0, tot = 0, t1 = ccount();
                while ((ccount() - t1) / 240000u < 150) {
                    int l = rf_wifi_rx(b, sizeof b, 5); if (l < 24) continue;
                    const uint32_t *hh = (const uint32_t *)rf_wifi_rx_header(), dl = (rf_wifi_rx_desc()[0] >> 12) & 0xFFF;
                    tot++; if (!memcmp(b + 4, f + 10, 6)) printf("    (to me: fc %02x%02x len %d a2 %02x%02x%02x%02x%02x%02x rssi %d)\n", b[0], b[1], l, b[10], b[11], b[12], b[13], b[14], b[15], (int8_t)(hh[0] & 0xFF)); if (b[0] != 0x80 && tot < 40) printf("    (any frame: fc %02x%02x len %d a1 %02x%02x%02x%02x%02x%02x a2 %02x%02x%02x%02x%02x%02x)\n", b[0], b[1], l, b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
                    if (memcmp(b + 10, bb, 6) || b[0] == 0x80) continue;                          /* from the AP only */
                    printf("  attempt %d: tx %d, switch %lu us, AP frame fc %02x%02x len %d rssi %d: %02x %02x %02x %02x %02x %02x\n", attempt, rc, (unsigned long)sw, b[0], b[1], l, (int8_t)(hh[0] & 0xFF), b[24], b[25], b[26], b[27], b[28], b[29]);
                    got++;
                }
                if (!got) printf("  attempt %d: tx %d, switch %lu us, nothing from the AP (%lu valid frames)\n", attempt, rc, (unsigned long)sw, (unsigned long)tot);
                else break;
            }
        } else if (!strcmp(v[0], "ring")) { const uint32_t (*r)[4] = rf_wifi_rx_ring(); for (int i = 0; i < 4; i++) printf("d%d %08x %08x %08x %08x\n", i, (unsigned)r[i][0], (unsigned)r[i][1], (unsigned)r[i][2], (unsigned)r[i][3]); }
        else if (!strcmp(v[0], "find") && n > 1) {              /* find <text> [ms]: report every receive buffer (headerless too) that contains text */
            uint32_t ms = n > 2 ? strtoul(v[2], 0, 0) : 2000, t0 = ccount(), hit = 0, seen = 0; const uint32_t (*r)[4] = rf_wifi_rx_ring(); unsigned tl = strlen(v[1]);
            while ((ccount() - t0) / 240000u < ms) for (int i = 0; i < 8; i++) {
                if (!(r[i][0] & 0x40000000u)) continue;
                const uint8_t *bf = (const uint8_t *)r[i][1]; seen++;
                for (unsigned j = 0; j + tl <= 400; j++) if (!memcmp(bf + j, v[1], tl)) {
                    hit++; if (hit <= 4) printf("  hit buf %d at %u: desc %08x hdr0 %08x hdr11 %08x a2 %02x%02x%02x%02x%02x%02x fc %02x%02x\n", i, j, (unsigned)r[i][0], (unsigned)((const uint32_t *)bf)[0], (unsigned)((const uint32_t *)bf)[11], bf[58], bf[59], bf[60], bf[61], bf[62], bf[63], bf[48], bf[49]);
                    break; }
                ((uint32_t *)r[i])[0] = ((uint32_t *)r[i])[3] = 0x80000000u | 1664;   /* re-arm */
            }
            printf("find %s: %lu hits in %lu buffers\n", v[1], (unsigned long)hit, (unsigned long)seen);
        } else if (!strcmp(v[0], "rftx")) printf("rftx %d\n", rf_tx_start());
        else if (!strcmp(v[0], "rfrx")) printf("rfrx %d\n", rf_rx_start());
        else if (!strcmp(v[0], "txinit")) printf("txinit %d\n", rf_wifi_tx_init());
        else if (!strcmp(v[0], "mac") && n > 1) {
            unsigned m[6]; if (parse_mac(v[1], m)) { printf("bad mac\n"); continue; }
            for (int i = 0; i < 6; i++) my[i] = (uint8_t)m[i];
            R32(0x60033040) = m[0] | m[1] << 8 | m[2] << 16 | m[3] << 24; R32(0x60033044) = m[4] | m[5] << 8;
            R32(0x60033060) = 0xFFFFFFFF; R32(0x60033064) = 0x0001FFFF;           /* mask bits + enable (bit 16) */
            printf("mac set %s: %08x %08x ack %08x\n", v[1], (unsigned)R32(0x60033040), (unsigned)R32(0x60033044), (unsigned)R32(0x60033064));
        } else if (!strcmp(v[0], "rx")) {
            uint32_t ms = n > 1 ? strtoul(v[1], 0, 0) : 3000, t0 = ccount(), got = 0, bad = 0, mine = 0, tagged = 0, byt[4] = { 0 };
            static uint8_t b[1700];
            while ((ccount() - t0) / 240000u < ms) {
                int l = rf_wifi_rx(b, sizeof b, 20); if (l <= 0) continue;
                const uint32_t *hh = (const uint32_t *)rf_wifi_rx_header(), dl = (rf_wifi_rx_desc()[0] >> 12) & 0xFFF;
                got++; byt[(b[0] >> 2) & 3]++;
                for (int j = 24; j + 9 <= l; j++) if (!memcmp(b + j, "RAWBEACON", 9)) { tagged++; break; }
                static const uint8_t peer[6] = { 0x30, 0x30, 0xf9, 0x5a, 0xc1, 0x11 }, peer2[6] = { 0x30, 0x30, 0xf9, 0x5a, 0xc1, 0x10 };
                if (l >= 16 && (!memcmp(b + 4, my, 6) || !memcmp(b + 10, peer, 6) || !memcmp(b + 10, peer2, 6))) {
                    mine++;
                    if (mine <= 12 && (b[0] & 0xFC) != 0x80) printf("  from/to peer: len %d rssi %d rate %u fc %02x%02x ra %02x%02x%02x%02x%02x%02x\n", l, (int8_t)(hh[0] & 0xFF), (unsigned)((hh[0] >> 8) & 0x1F), b[0], b[1], b[4], b[5], b[6], b[7], b[8], b[9]);
                }
                if (n > 2 && got <= 12) {
                    const uint8_t *h = rf_wifi_rx_header();
                    printf("rx len %3d rssi %d rate %u fc %02x%02x ra %02x:%02x:%02x:%02x:%02x:%02x hdr:", l, (int8_t)(hh[0] & 0xFF), (unsigned)((hh[0] >> 8) & 0x1F), b[0], b[1], b[4], b[5], b[6], b[7], b[8], b[9]);
                    for (int j = 0; j < 12; j++) printf(" %08x", (unsigned)((const uint32_t *)h)[j]);
                    const uint32_t *d = rf_wifi_rx_desc(); printf(" | desc %08x %08x %08x %08x\n", (unsigned)d[0], (unsigned)d[1], (unsigned)d[2], (unsigned)d[3]);
                }
            }
            printf("rx tagged %lu to-me %lu got %lu (headerless %lu) mgmt %lu ctrl %lu data %lu in %lu ms\n", (unsigned long)tagged, (unsigned long)mine, (unsigned long)got, (unsigned long)bad, (unsigned long)byt[0], (unsigned long)byt[1], (unsigned long)byt[2], (unsigned long)ms);
        } else if (!strcmp(v[0], "rd") && n > 1) {
            uint32_t a = strtoul(v[1], 0, 0), c = n > 2 ? strtoul(v[2], 0, 0) : 1;
            for (uint32_t i = 0; i < c; i++) printf("%08x: %08x\n", (unsigned)(a + 4 * i), (unsigned)R32(a + 4 * i));
        } else if (!strcmp(v[0], "wr") && n > 2) R32(strtoul(v[1], 0, 0)) = strtoul(v[2], 0, 0);
        else if (!strcmp(v[0], "tx") && n > 1) {
            static uint8_t f[300]; int l = hexbin(v[1], f, sizeof f);
            printf("tx %d\n", rf_wifi_tx(f, l));
        }
        printf("ok\n");
    }
}
