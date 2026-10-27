/* Station: scan, open-system join, data in and out. Driven by wifi_poll() from the main loop; scan and join pump the radio themselves. */
#include <string.h>
#include "wifi.h"
#include "wifi_mac.h"
#include "sys.h"

#define KEEPALIVE_MS 20000u                 /* the AP drops silent stations after minutes (IDF: 300 s) */

void (*wifi_eth_rx)(const uint8_t *frame, unsigned n);

enum { DOWN, AUTHING, ASSOCING, UP };
static struct {
    uint8_t me[6], bssid[6], chan, state; uint16_t seq, last_seq, beacon_tu;
    uint32_t last_beacon, last_tx;
    wifi_ap_t *out; unsigned max, n; const char *want;       /* scan collection; want = only this SSID (join) */
} s;
static uint8_t fb[1600];
static unsigned data_rate = WIFI_RATE_12M;
void wifi_set_data_rate(unsigned rate) { data_rate = rate; }

static int tx(unsigned len) { s.last_tx = time_ms(); return wifi_mac_tx(fb, len); }
static uint16_t next_seq(void) { return s.seq++ & 0xFFF; }

static void down(void) { s.state = DOWN; }

static void on_frame(const wifi_rx_t *r)
{
    const uint8_t *f = r->data; unsigned len = r->len;
    if (len < 24) return;
    uint16_t sq = (uint16_t)((f[22] | f[23] << 8) >> 4);
    switch (f[0]) {
    case 0x80: case 0x50: {                                  /* beacon, probe response */
        wifi_ap_t ap;
        if (wifi_parse_beacon(f, len, &ap)) return;
        if (!ap.chan) ap.chan = r->chan;
        if (s.state == UP && !memcmp(ap.bssid, s.bssid, 6)) { s.last_beacon = time_ms(); return; }
        if (!s.out) return;
        if (s.want && strcmp(ap.ssid, s.want)) return;
        ap.rssi = r->rssi;
        for (unsigned i = 0; i < s.n; i++) if (!memcmp(s.out[i].bssid, ap.bssid, 6)) { s.out[i] = ap; return; }
        if (s.n < s.max) s.out[s.n++] = ap;
        return; }
    case 0xB0:                                               /* authentication: ours, transaction 2, success */
        if (s.state == AUTHING && len >= 30 && !memcmp(f + 10, s.bssid, 6) && !memcmp(f + 4, s.me, 6) && f[24] == 0 && f[26] == 2 && !f[28] && !f[29]) s.state = ASSOCING;
        return;
    case 0x10:                                               /* association response: success */
        if (s.state == ASSOCING && len >= 30 && !memcmp(f + 10, s.bssid, 6) && !memcmp(f + 4, s.me, 6) && !f[26] && !f[27]) { s.state = UP; s.last_beacon = time_ms(); }
        return;
    case 0xC0: case 0xA0:                                    /* deauthentication, disassociation: only from our AP */
        if (s.state != DOWN && !memcmp(f + 10, s.bssid, 6) && !memcmp(f + 16, s.bssid, 6)) down();
        return;
    default: break;
    }
    if (s.state != UP || (f[0] & 0x0C) != 0x08) return;
    if ((f[1] & 0x08) && sq == s.last_seq) return;           /* retransmission of a frame we have seen (no hardware ACK yet) */
    s.last_seq = sq;
    static uint8_t eth[1600];
    int n = wifi_data_to_eth(f, len, s.bssid, eth, sizeof eth);
    if (n > 0 && wifi_eth_rx) wifi_eth_rx(eth, (unsigned)n);
}

static void pump(uint32_t ms)                                /* dispatch received frames for ms */
{
    uint32_t t0 = time_ms(); wifi_rx_t r;
    do { while (wifi_mac_rx(&r, 0)) on_frame(&r); } while (time_ms() - t0 < ms);
}

int wifi_init(const uint8_t mac[6]) { memcpy(s.me, mac, 6); s.chan = 6; return wifi_mac_init(mac); }

static int scan(wifi_ap_t *out, unsigned max, unsigned *n, const char *stop_ssid)
{
    s.out = out; s.max = max; s.n = 0; s.want = stop_ssid;
    for (uint8_t ch = 1; ch <= 13; ch++) {
        int rc = wifi_mac_set_channel(ch); if (rc) break;
        s.chan = ch;
        unsigned l = wifi_build_probe_req(fb, s.me, stop_ssid, next_seq());
        tx(l);
        pump(120);
        if (stop_ssid) for (unsigned i = 0; i < s.n; i++) if (!strcmp(out[i].ssid, stop_ssid)) goto done;
    }
done:
    s.out = 0; s.want = 0;
    if (n) *n = s.n;
    return 0;
}

int wifi_scan(wifi_ap_t *out, unsigned max, unsigned *n) { down(); return scan(out, max, n, 0); }

static int step(uint8_t want, unsigned (*build)(void), uint32_t wait_ms)   /* send up to 3 times, wait for the state to change */
{
    for (int i = 0; i < 3; i++) {
        tx(build());
        uint32_t t0 = time_ms();
        while (time_ms() - t0 < wait_ms) { pump(2); if (s.state == want) return 0; }
    }
    return -1;
}
static const char *join_ssid;
static unsigned b_auth(void) { return wifi_build_auth(fb, s.me, s.bssid, next_seq()); }
static unsigned b_assoc(void) { return wifi_build_assoc_req(fb, s.me, s.bssid, join_ssid, next_seq()); }

int wifi_sta_join(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (pass) return -2;                                     /* WPA2: task 10 */
    uint32_t t0 = time_ms();
    wifi_ap_t found[8]; unsigned n = 0;
    down();
    scan(found, 8, &n, ssid);
    unsigned k = 0; while (k < n && strcmp(found[k].ssid, ssid)) k++;
    if (k == n || found[k].sec) return -1;
    memcpy(s.bssid, found[k].bssid, 6); s.beacon_tu = found[k].beacon_tu ? found[k].beacon_tu : 100;
    wifi_mac_set_channel(found[k].chan); s.chan = found[k].chan;
    join_ssid = ssid; s.state = AUTHING;
    if (step(ASSOCING, b_auth, 100) || time_ms() - t0 > timeout_ms) { down(); return -1; }
    if (step(UP, b_assoc, 100)) { down(); return -1; }
    s.last_seq = 0xFFFF;
    return 0;
}

int wifi_sta_up(void) { return s.state == UP; }

void wifi_poll(void)
{
    pump(0);
    if (s.state != UP) return;
    if (time_ms() - s.last_beacon > 3u * s.beacon_tu * 1024u / 1000u + 1000u) { down(); return; }     /* 3 beacon intervals + 1 s */
    if (time_ms() - s.last_tx > KEEPALIVE_MS) tx(wifi_build_null(fb, s.me, s.bssid, next_seq()));
}

int wifi_send_eth(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n)
{
    if (s.state != UP || n > 1500) return -1;
    unsigned l = wifi_build_data(fb, s.me, s.bssid, dst, ethertype, p, n, next_seq());
    s.last_tx = time_ms();
    return wifi_mac_tx_rate(fb, l, data_rate);          /* data at 12 Mbit/s: DSSS frames over ~1200 B are not heard (docs/WIFI_MAC.md) */
}
