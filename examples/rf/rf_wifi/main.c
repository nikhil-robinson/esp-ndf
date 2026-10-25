/* 4.2: raw 802.11 over the Wi-Fi MAC from our own code (rf.h: rf_wifi_*), no vendor stack.
 * Two boards: one runs "tx", the other "rx" (tools/rf_wifi_test.py drives both).
 *   tx [count]   send our tagged frame count times (0 = continuous for ~15 s)
 *   rx [ms]      receive into our own buffers; print the first frames and a count */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include "rf.h"
#include <stdlib.h>
#include <string.h>

static char line[64];
/* a minimal 802.11 data frame; addr2 = lab source id 02:4e:44:46:00:01 */
static const uint8_t frame[100] = {
    0x88,0x01,0x2c,0x00, 0x18,0xfe,0x34,0x01,0x02,0x03, 0x02,0x4e,0x44,0x46,0x00,0x01,
    0xff,0xff,0xff,0xff,0xff,0xff, 0x60,0x47 };

int main(void)
{
    delay_ms(300);
    printf("rf_wifi ready open %d\n", rf_open());
    int k = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch != '\n') { if (k < (int)sizeof line - 1) line[k++] = (char)ch; continue; }
        line[k] = 0; k = 0;
        char *v[4]; int n = 0;
        for (char *p = line; *p && n < 4; ) { while (*p == ' ') *p++ = 0; if (!*p) break; v[n++] = p; while (*p && *p != ' ') p++; }
        if (!n) { printf("ok\n"); continue; }
        if (!strcmp(v[0], "tx")) {
            int rc = rf_wifi_tx_init();
            uint32_t c = n > 1 ? strtoul(v[1], 0, 0) : 1, ok = 0;
            if (c == 0) { uint32_t t0 = ccount(); while ((ccount() - t0) / 240000u < 15000) { ok += rf_wifi_tx(frame, sizeof frame) == RF_OK; c++; } }
            else for (uint32_t i = 0; i < c; i++) ok += rf_wifi_tx(frame, sizeof frame) == RF_OK;
            printf("tx init %d ok %lu/%lu\n", rc, (unsigned long)ok, (unsigned long)c);
        } else if (!strcmp(v[0], "rx")) {
            int rc = rf_wifi_rx_init();
            uint8_t b[300]; uint32_t ms = n > 1 ? strtoul(v[1], 0, 0) : 3000, t0 = ccount(), got = 0, ours = 0;
            static const uint8_t id[6] = { 0x02,0x4e,0x44,0x46,0x00,0x01 };
            while ((ccount() - t0) / 240000u < ms) {
                int l = rf_wifi_rx(b, sizeof b, 50); if (l <= 0) continue;
                got++;
                if (l >= 16 && !memcmp(b + 10, id, 6)) ours++;
                if (got <= 6) { printf("rx len %d:", l); for (int j = 0; j < l && j < 24; j++) printf(" %02x", b[j]); printf("\n"); }
            }
            printf("rx init %d got %lu ours %lu in %lu ms\n", rc, (unsigned long)got, (unsigned long)ours, (unsigned long)ms);
        }
        printf("ok\n");
    }
}
