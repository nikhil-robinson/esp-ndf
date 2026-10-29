/* Host test + fuzzer of components/wifi/{ie.c,frame.c}: fixed vectors (beacon fields, builders, data <-> Ethernet), then random and mutated frames:
 * nothing may crash or read past the frame, and a data frame that converts must fit the output buffer.  usage: fuzz_wifi_frames [rounds] [seed] */
#include "wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long x = 1;
static unsigned rnd(void) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return (unsigned)x; }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
    unsigned long rounds = argc > 1 ? strtoul(argv[1], 0, 0) : 200000; x = argc > 2 ? strtoul(argv[2], 0, 0) | 1 : 7;
    const uint8_t me[6] = { 2, 0x4e, 0x44, 0x46, 0, 2 }, ap_[6] = { 0x30, 0x30, 0xf9, 0x5a, 0xc1, 0x11 };

    /* beacon vector: SSID "T", DS channel 11, privacy bit set, interval 100 TU */
    uint8_t b[64] = { 0x80 }; memcpy(b + 16, ap_, 6); b[32] = 100; b[34] = 0x11;
    uint8_t *ie = b + 36; ie[0] = 0; ie[1] = 1; ie[2] = 'T'; ie[3] = 3; ie[4] = 1; ie[5] = 11;
    wifi_ap_t ap;
    CHECK(wifi_parse_beacon(b, 36 + 6, &ap) == 0 && !strcmp(ap.ssid, "T") && ap.chan == 11 && ap.sec == 1 && ap.beacon_tu == 100 && !memcmp(ap.bssid, ap_, 6));
    CHECK(wifi_parse_beacon(b, 36 + 5, &ap) == -1);          /* the DS element runs past the frame: rejected */
    ie[1] = 33; CHECK(wifi_parse_beacon(b, 64, &ap) == -1);  /* SSID longer than 32 */
    ie[1] = 1; b[0] = 0x40; CHECK(wifi_parse_beacon(b, 42, &ap) == -1);   /* a probe request is not a beacon */

    /* builders */
    uint8_t f[200]; unsigned n;
    n = wifi_build_probe_req(f, me, NULL, 5);
    CHECK(n == 24 + 2 + 6 && f[0] == 0x40 && f[24] == 0 && f[25] == 0 && f[22] == 0x50 && f[23] == 0);
    n = wifi_build_auth(f, me, ap_, 1); CHECK(n == 30 && f[0] == 0xB0 && f[26] == 1 && !memcmp(f + 4, ap_, 6) && !memcmp(f + 10, me, 6));
    n = wifi_build_assoc_req(f, me, ap_, "net", 0, 2); CHECK(n == 24 + 4 + 5 + 6 + 9 && f[0] == 0 && f[28] == 0 && f[29] == 3 && !memcmp(f + 30, "net", 3));
    n = wifi_build_deauth(f, me, ap_, 3, 1); CHECK(n == 26 && f[0] == 0xC0 && f[24] == 3);

    n = wifi_build_assoc_req(f, me, ap_, "net", 1, 2); CHECK(n == 24 + 4 + 5 + 6 + 22 + 9 && f[n - 31] == 48 && f[n - 30] == 20 && f[n - 9] == 0xdd);
    uint8_t rb2[80] = { 0x80 }; memcpy(rb2 + 16, ap_, 6); rb2[34] = 0x11; rb2[36] = 0; rb2[37] = 1; rb2[38] = 'T'; rb2[39] = 48; rb2[40] = 20; rb2[41] = 1;   /* RSN element => WPA2 */
    CHECK(wifi_parse_beacon(rb2, 36 + 3 + 22, &ap) == 0 && ap.sec == 2);

    /* data frame to the AP, and a from-DS frame converted to Ethernet */
    const uint8_t dst[6] = { 1, 2, 3, 4, 5, 6 }, pl[4] = { 9, 8, 7, 6 };
    n = wifi_build_data(f, me, ap_, dst, 0x0806, pl, 4, 7);
    CHECK(n == 24 + 8 + 4 && f[0] == 0x08 && f[1] == 0x01 && !memcmp(f + 16, dst, 6) && f[30] == 0x08 && f[31] == 0x06 && !memcmp(f + 32, pl, 4));
    uint8_t d[100], eth[100]; memcpy(d, f, n);
    d[1] = 0x02; memcpy(d + 4, me, 6); memcpy(d + 10, ap_, 6); memcpy(d + 16, dst, 6);      /* from DS: DA = us, TA = AP, SA = dst */
    int e = wifi_data_to_eth(d, n, ap_, eth, sizeof eth);
    CHECK(e == 14 + 4 && !memcmp(eth, me, 6) && !memcmp(eth + 6, dst, 6) && eth[12] == 0x08 && eth[13] == 0x06 && !memcmp(eth + 14, pl, 4));
    CHECK(wifi_data_to_eth(d, n, me, eth, sizeof eth) == -1);        /* transmitter is not our AP */
    CHECK(wifi_data_to_eth(d, n, ap_, eth, 15) == -1);               /* output too small */
    d[1] |= 0x40; CHECK(wifi_data_to_eth(d, n, ap_, eth, sizeof eth) == -1);   /* protected frame: not for the open path */

    /* access point side: beacon / probe response parse back through the station's parser, response builders, to-DS data to Ethernet */
    uint8_t bc[6] = { 255, 255, 255, 255, 255, 255 };
    n = wifi_build_beacon(f, bc, ap_, "net", 9, 0, 0x0102030405060708ull, 3, 0);
    CHECK(f[0] == 0x80 && !memcmp(f + 16, ap_, 6) && f[24] == 8 && f[31] == 1 && !wifi_parse_beacon(f, n, &ap) && !strcmp(ap.ssid, "net") && ap.chan == 9 && ap.sec == 0 && ap.beacon_tu == 100);
    n = wifi_build_beacon(f, me, ap_, "net", 9, 1, 0, 3, 1); CHECK(f[0] == 0x50 && !memcmp(f + 4, me, 6) && !wifi_parse_beacon(f, n, &ap) && ap.sec == 2);
    n = wifi_build_beacon(f, bc, ap_, "0123456789012345678901234567890123456789", 9, 0, 0, 0, 0); CHECK(n <= sizeof f && !wifi_parse_beacon(f, n, &ap) && strlen(ap.ssid) <= 32);   /* over-long SSID is cut, not overrun */
    n = wifi_build_auth_resp(f, me, ap_, 17, 4); CHECK(n == 30 && f[0] == 0xB0 && f[26] == 2 && f[28] == 17 && !memcmp(f + 4, me, 6) && !memcmp(f + 10, ap_, 6) && !memcmp(f + 16, ap_, 6));
    n = wifi_build_assoc_resp(f, me, ap_, 0, 3, 1, 5); CHECK(f[0] == 0x10 && f[24] == 0x11 && f[28] == 3 && f[29] == 0xC0 && n == 30 + 6);
    n = wifi_build_deauth_ap(f, me, ap_, 15, 6); CHECK(n == 26 && f[0] == 0xC0 && f[24] == 15 && !memcmp(f + 4, me, 6) && !memcmp(f + 16, ap_, 6));
    n = wifi_build_data_fromds(f, ap_, dst, me, 0x0800, pl, 4, 7); CHECK(f[1] == 0x02 && !memcmp(f + 4, dst, 6) && !memcmp(f + 10, ap_, 6) && !memcmp(f + 16, me, 6) && n == 24 + 8 + 4);
    memcpy(d, f, n); d[1] = 0x01; memcpy(d + 4, ap_, 6); memcpy(d + 10, me, 6); memcpy(d + 16, dst, 6);            /* to DS: RA = AP, SA = us, DA = dst */
    e = wifi_ap_data_to_eth(d, n, ap_, eth, sizeof eth); CHECK(e == 14 + 4 && !memcmp(eth, dst, 6) && !memcmp(eth + 6, me, 6) && eth[12] == 0x08 && !memcmp(eth + 14, pl, 4));
    CHECK(wifi_ap_data_to_eth(d, n, me, eth, sizeof eth) == -1 && wifi_ap_data_to_eth(d, n, ap_, eth, 15) == -1);    /* not for us / output too small */
    d[1] |= 0x40; CHECK(wifi_ap_data_to_eth(d, n, ap_, eth, sizeof eth) == -1);

    /* random and mutated input; guard bytes behind the frame catch overreads under ASan */
    for (unsigned long i = 0; i < rounds && !fails; i++) {
        unsigned len = rnd() % 120; uint8_t *buf = malloc(len ? len : 1); uint8_t out[64];
        for (unsigned k = 0; k < len; k++) buf[k] = (uint8_t)rnd();
        if (len > 36 && (rnd() & 1)) { buf[0] = (rnd() & 1) ? 0x80 : 0x50; unsigned p = 36; while (p + 2 <= len && (rnd() & 7)) { buf[p + 1] = (uint8_t)(rnd() % 40); p += 2 + buf[p + 1]; } }
        wifi_ap_t a; if (!wifi_parse_beacon(buf, len, &a)) { CHECK(strlen(a.ssid) <= 32); }
        if (len > 24 && (rnd() & 1)) { buf[0] = (rnd() & 1) ? 0x08 : 0x88; buf[1] = 0x02; memcpy(buf + 10, ap_, 6); if (len > 32 && (rnd() & 1)) memcpy(buf + 24, "\xAA\xAA\x03\0\0\0", 6); }
        int r = wifi_data_to_eth(buf, len, ap_, out, sizeof out); CHECK(r == -1 || (r >= 14 && r <= (int)sizeof out));
        if (len > 24 && (rnd() & 1)) { buf[0] = (rnd() & 1) ? 0x08 : 0x88; buf[1] = 0x01; memcpy(buf + 4, ap_, 6); if (len > 32 && (rnd() & 1)) memcpy(buf + 24, "\xAA\xAA\x03\0\0\0", 6); }
        r = wifi_ap_data_to_eth(buf, len, ap_, out, sizeof out); CHECK(r == -1 || (r >= 14 && r <= (int)sizeof out));
        const uint8_t *v; wifi_ie_find(buf, len, (uint8_t)rnd(), &v);
        free(buf);
    }
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("fuzz_wifi_frames: ok (%lu rounds)\n", rounds);
    return 0;
}
