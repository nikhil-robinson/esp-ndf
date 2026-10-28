/* Station: scan, join (open or WPA2-PSK / CCMP), data in and out. Driven by wifi_poll() from the main loop; scan and join pump the radio themselves. */
#include <string.h>
#include "wifi.h"
#include "wifi_mac.h"
#include "wpa.h"
#include "rng.h"
#include "sys.h"

#define KEEPALIVE_MS 20000u                 /* the AP drops silent stations after minutes (IDF: 300 s) */

void (*wifi_eth_rx)(const uint8_t *frame, unsigned n);

enum { DOWN, AUTHING, ASSOCING, HANDSHAKE, UP };
static struct {
    uint8_t me[6], bssid[6], chan, state; uint16_t seq, last_seq, beacon_tu;
    uint32_t last_beacon, last_tx;
    wifi_ap_t *out; unsigned max, n; const char *want;       /* scan collection; want = only this SSID (join) */
    uint8_t secure;                                          /* WPA2 network */
} s;
static struct {                                              /* WPA2 state; wiped when the link goes down */
    uint8_t pmk[32], ptk[48], anonce[32], snonce[32], gtk[16], gtk_id;
    uint64_t replay, tx_pn, rx_pn[2][8];                     /* last EAPOL replay counter; CCMP packet numbers: transmit, receive [group][TID] */
    uint8_t have_ptk, keys, have_gtk, have_replay, msg1_seen;
} k;
#define KCK (k.ptk)
#define KEK (k.ptk + 16)
#define TK  (k.ptk + 32)
static uint8_t fb[1600];
static unsigned data_rate = WIFI_RATE_12M;
void wifi_set_data_rate(unsigned rate) { data_rate = rate; }

static int tx(unsigned len) { s.last_tx = time_ms(); return wifi_mac_tx(fb, len); }
static uint16_t next_seq(void) { return s.seq++ & 0xFFF; }

static void zero(void *p, unsigned n) { volatile uint8_t *q = p; while (n--) *q++ = 0; }
static void down(void) { wifi_mac_clear_keys(); s.state = DOWN; zero(&k.ptk, sizeof k.ptk); zero(&k.gtk, sizeof k.gtk); memset(k.rx_pn, 0, sizeof k.rx_pn); k.tx_pn = 0; k.have_ptk = k.keys = k.have_gtk = k.have_replay = k.msg1_seen = 0; }

static int send_eth_rate(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n, int encrypt, unsigned rate);
static int send_eth(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n, int encrypt) { return send_eth_rate(dst, ethertype, p, n, encrypt, data_rate); }
static int send_eth_rate(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n, int encrypt, unsigned rate)
{
    unsigned l = wifi_build_data(fb, s.me, s.bssid, dst, ethertype, p, n, next_seq());
    if (encrypt) {                                           /* QoS data (as the IDF station sends): header 26 | CCMP header 8 | LLC/SNAP + payload | MIC 8 */
        memmove(fb + 26 + 8, fb + 24, l - 24); fb[0] = 0x88; fb[1] |= 0x40; fb[24] = fb[25] = 0;
        int c = ccmp_encrypt(TK, fb, 26, ++k.tx_pn, 0, fb + 26, l - 24); if (c < 0) return -1;
        l = 26 + (unsigned)c;
    }
    s.last_tx = time_ms();
    return wifi_mac_tx_rate(fb, l, rate);               /* data at 12 Mbit/s: DSSS frames over ~1200 B are not heard (docs/WIFI_MAC.md) */
}

/* --- WPA2 4-way and group key handshake (station side) --- */
static const uint8_t rsn_ie[22] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };

static void hs_send(uint16_t info, uint64_t replay, const uint8_t *nonce, const uint8_t *data, unsigned dl)
{
    uint8_t e[4 + 95 + 24]; unsigned body = 95 + dl;
    memset(e, 0, sizeof e);
    e[0] = 2; e[1] = 3; e[2] = (uint8_t)(body >> 8); e[3] = (uint8_t)body; e[4] = 2; e[5] = (uint8_t)(info >> 8); e[6] = (uint8_t)info;
    for (int i = 0; i < 8; i++) e[9 + i] = (uint8_t)(replay >> (56 - 8 * i));
    if (nonce) memcpy(e + 17, nonce, 32);
    e[97] = (uint8_t)(dl >> 8); e[98] = (uint8_t)dl; if (dl) memcpy(e + 99, data, dl);
    uint8_t m[20]; wpa_hmac_sha1(KCK, 16, e, 4 + body, m); memcpy(e + 81, m, 16);
    send_eth_rate(s.bssid, 0x888E, e, 4 + body, 0, WIFI_RATE_1M);       /* the handshake at 1 Mbit/s: the AP misses 12 Mbit/s frames now and then (measured: 6 of 6 joins instead of about half) */
}

static int take_gtk(const uint8_t *d, unsigned n)                /* GTK KDE inside the unwrapped key data; 0 = found and stored */
{
    for (unsigned i = 0; i + 2 <= n; ) {
        unsigned l = d[i + 1];
        if (i + 2 + l > n) return -1;
        if (d[i] == 0xdd && l >= 6 + 16 && !memcmp(d + i + 2, "\x00\x0f\xac\x01", 4)) { memcpy(k.gtk, d + i + 8, 16); k.gtk_id = d[i + 6] & 3; k.have_gtk = 1; memset(k.rx_pn[1], 0, sizeof k.rx_pn[1]); return 0; }
        i += 2 + l;
    }
    return -1;
}

static void eapol_rx(const uint8_t *f, unsigned n)
{
    eapol_key_t e;
    if (!s.secure || eapol_key_parse(f, n, &e) || (e.info & 7) != 2) return;
    if ((e.info & EAPOL_INFO_PAIRWISE) && (e.info & EAPOL_INFO_ACK) && !(e.info & EAPOL_INFO_MIC)) {          /* message 1 */
        if (s.state != HANDSHAKE) return;
        if (++k.msg1_seen > 2 && k.have_ptk) { down(); return; }                      /* the AP has asked three times: our message 2 is being refused (wrong passphrase) */
        memcpy(k.anonce, e.nonce, 32); rng_fill(k.snonce, 32);
        wpa_ptk(k.pmk, s.bssid, s.me, k.anonce, k.snonce, k.ptk); k.have_ptk = 1;
        k.replay = e.replay; k.have_replay = 1;
        hs_send(0x010a, e.replay, k.snonce, rsn_ie, sizeof rsn_ie);
        return;
    }
    if (!k.have_ptk || eapol_key_check(KCK, f, n)) return;                           /* messages 3 and the group key need a good MIC */
    if (k.have_replay && e.replay <= k.replay && !(k.keys && e.replay == k.replay && (e.info & EAPOL_INFO_PAIRWISE))) return;   /* replayed (a retransmitted message 3 after install is answered below) */
    if (!(e.info & EAPOL_INFO_ACK) || !(e.info & EAPOL_INFO_SECURE) || !(e.info & EAPOL_INFO_ENCDATA)) return;
    if (e.info & EAPOL_INFO_PAIRWISE) {                                               /* message 3 */
        if (!(e.info & EAPOL_INFO_INSTALL) || memcmp(e.nonce, k.anonce, 32)) return;  /* a different ANonce than message 1 is an attack or a restart */
        if (k.keys) { hs_send(0x030a, e.replay, 0, 0, 0); return; }                  /* retransmitted message 3: message 4 was lost */
        uint8_t kd[80]; int kl = aes_unwrap(KEK, e.data, e.data_len, kd);
        if (kl < 0 || take_gtk(kd, (unsigned)kl)) { zero(kd, sizeof kd); return; }    /* nothing installed on a bad message 3 */
        k.replay = e.replay; hs_send(0x030a, e.replay, 0, 0, 0);
        wifi_mac_set_key(0, s.bssid, TK, 0); wifi_mac_set_key(1, s.bssid, k.gtk, k.gtk_id);
        k.keys = 1; k.tx_pn = 0; memset(k.rx_pn, 0, sizeof k.rx_pn); s.state = UP; s.last_beacon = time_ms();
        zero(kd, sizeof kd);
    } else if (s.state == UP) {                                                       /* group key update */
        uint8_t kd[80]; int kl = aes_unwrap(KEK, e.data, e.data_len, kd);
        if (kl < 0 || take_gtk(kd, (unsigned)kl)) { zero(kd, sizeof kd); return; }
        wifi_mac_set_key(1, s.bssid, k.gtk, k.gtk_id);
        k.replay = e.replay; hs_send(0x0302, e.replay, 0, 0, 0);
        zero(kd, sizeof kd);
    }
}

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
        if (s.state >= HANDSHAKE && !memcmp(ap.bssid, s.bssid, 6)) { s.last_beacon = time_ms(); return; }
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
        if (s.state == ASSOCING && len >= 30 && !memcmp(f + 10, s.bssid, 6) && !memcmp(f + 4, s.me, 6) && !f[26] && !f[27]) { s.state = s.secure ? HANDSHAKE : UP; s.last_beacon = time_ms(); k.msg1_seen = 0; }
        return;
    case 0xC0: case 0xA0:                                    /* deauthentication, disassociation: only from our AP */
        if (s.state != DOWN && !memcmp(f + 10, s.bssid, 6) && !memcmp(f + 16, s.bssid, 6)) down();
        return;
    default: break;
    }
    if (s.state < HANDSHAKE || (f[0] & 0x0C) != 0x08) return;
    if ((f[1] & 0x08) && sq == s.last_seq) return;           /* retransmission of a frame we have seen (no hardware ACK yet) */
    s.last_seq = sq;
    static uint8_t eth[1600];
    if (f[1] & 0x40) {                                       /* CCMP */
        unsigned hl = 24 + ((f[0] & 0x80) ? 2 : 0), grp = f[4] & 1, tid = (f[0] & 0x80) ? f[24] & 7 : 0; uint64_t pn;
        if (!k.keys || memcmp(f + 10, s.bssid, 6) || (grp && !k.have_gtk) || len > 1600) return;
        uint8_t *pl = (uint8_t *)f; int d;                  /* the MAC's receive buffer is ours until the next wifi_mac_rx */
        if (r->hw_decrypted) {                               /* decrypted and MIC-checked by the MAC (key table in wifi_mac.c): CCMP header kept, MIC gone */
            if (len < hl + 8) return;
            pn = (uint64_t)pl[hl] | (uint64_t)pl[hl + 1] << 8 | (uint64_t)pl[hl + 4] << 16 | (uint64_t)pl[hl + 5] << 24 | (uint64_t)pl[hl + 6] << 32 | (uint64_t)pl[hl + 7] << 40;
            d = (int)(len - hl - 8);
        } else {                                             /* not in the key table: decrypt in place here */
            if (len < hl + 16) return;
            d = ccmp_decrypt(grp ? k.gtk : TK, pl, hl, pl + hl, len - hl, &pn);
        }
        if (d < 0 || pn <= k.rx_pn[grp][tid]) return;        /* bad MIC or a replayed packet number */
        k.rx_pn[grp][tid] = pn;
        memmove(pl + hl, pl + hl + 8, (unsigned)d); pl[1] &= (uint8_t)~0x40;
        int n = wifi_data_to_eth(pl, hl + (unsigned)d, s.bssid, eth, sizeof eth);
        if (n > 0 && !memcmp(eth + 6, s.me, 6)) return;      /* the AP repeats our own broadcasts back to us */
        if (n > 0 && wifi_eth_rx) wifi_eth_rx(eth, (unsigned)n);
        return;
    }
    int n = wifi_data_to_eth(f, len, s.bssid, eth, sizeof eth);
    if (n < 14 || !memcmp(eth + 6, s.me, 6)) return;
    if (s.secure && (eth[12] != 0x88 || eth[13] != 0x8E)) return;      /* on a protected network only EAPOL may arrive in the clear */
    if (eth[12] == 0x88 && eth[13] == 0x8E) { eapol_rx(eth + 14, (unsigned)n - 14); return; }
    if (wifi_eth_rx) wifi_eth_rx(eth, (unsigned)n);
}

static void pump(uint32_t ms)                                /* dispatch received frames for ms */
{
    uint32_t t0 = time_ms(); wifi_rx_t r;
    do { while (wifi_mac_rx(&r, 0)) on_frame(&r); } while (time_ms() - t0 < ms);
}

int wifi_init(const uint8_t mac[6]) { memcpy(s.me, mac, 6); s.chan = 6; return wifi_mac_init(mac); }
const uint8_t *wifi_mac_addr(void) { return s.me; }

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
static unsigned b_assoc(void) { return wifi_build_assoc_req(fb, s.me, s.bssid, join_ssid, s.secure, next_seq()); }

int wifi_sta_join(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    uint32_t t0 = time_ms();
    wifi_ap_t found[8]; unsigned n = 0;
    down();
    scan(found, 8, &n, ssid);
    unsigned i = 0; while (i < n && strcmp(found[i].ssid, ssid)) i++;
    if (i == n) return -1;
    if (pass ? found[i].sec != 2 : found[i].sec) return -2;  /* open join to a protected network, or a passphrase for an open / non-WPA2 one */
    s.secure = pass != 0;
    if (pass) wpa_pmk(pass, (const uint8_t *)ssid, (unsigned)strlen(ssid), k.pmk);
    memcpy(s.bssid, found[i].bssid, 6); s.beacon_tu = found[i].beacon_tu ? found[i].beacon_tu : 100;
    wifi_mac_set_channel(found[i].chan); s.chan = found[i].chan;
    join_ssid = ssid; s.state = AUTHING;
    if (step(ASSOCING, b_auth, 100) || time_ms() - t0 > timeout_ms) { down(); return -1; }
    if (step(s.secure ? HANDSHAKE : UP, b_assoc, 100)) { down(); return -1; }
    s.last_seq = 0xFFFF;
    if (s.secure) {
        uint32_t h0 = time_ms();
        while (s.state == HANDSHAKE && time_ms() - h0 < 3000) pump(2);
        if (s.state != UP) { down(); return -3; }
    }
    return 0;
}

int wifi_sta_up(void) { return s.state == UP; }

void wifi_poll(void)
{
    pump(0);
    if (s.state != UP) return;
    if (time_ms() - s.last_beacon > 3u * s.beacon_tu * 1024u / 1000u + 1000u) { down(); return; }     /* 3 beacon intervals + 1 s */
    if (time_ms() - s.last_tx > KEEPALIVE_MS && !s.secure) tx(wifi_build_null(fb, s.me, s.bssid, next_seq()));   /* ponytail: a protected link keeps itself alive with traffic; a CCMP null frame is not built yet */
}

int wifi_send_eth(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n)
{
    if (s.state != UP || n > 1500) return -1;
    return send_eth(dst, ethertype, p, n, s.secure);
}
