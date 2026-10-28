/* Access point: beacons, probe / authentication / association, WPA2-PSK authenticator (4-way handshake, one fixed group key), data both ways.
 * Open-system authentication keeps no state (a short list of recent authentications), stations are allocated at association. Received
 * protected frames are decrypted in software with the pairwise key (the MAC's key table holds one station). Driven by wifi_ap_poll(). */
#include <string.h>
#include "wifi.h"
#include "wifi_mac.h"
#include "wpa.h"
#include "rng.h"
#include "sys.h"
#include "rf.h"

#ifndef NDF_WIFI_MAX_STA
#define NDF_WIFI_MAX_STA 4
#endif
#define BEACON_US     102400u                /* 100 TU: what the beacons advertise */
#define HS_TIMEOUT_MS 5000u                  /* a station that does not finish the handshake is dropped */
#define HS_RETRY_MS   1000u
#define IDLE_MS       300000u                /* silent stations are dropped (IDF: 300 s) */
#define AUTH_MS       5000u                  /* an authentication is good for association this long */
#define PEND_N        8

enum { FREE, ASSOC, HS2, HS4, UP };          /* HS2: message 1 sent, waiting for 2; HS4: message 3 sent, waiting for 4 */
typedef struct {
    uint8_t mac[6], state, tries; uint16_t aid, last_seq;
    uint32_t last_rx, t0, t_msg;
    uint64_t replay, tx_pn, rx_pn[8];
    uint8_t anonce[32], ptk[48];
} sta_t;
#define KCK(s) ((s)->ptk)
#define KEK(s) ((s)->ptk + 16)
#define TK(s)  ((s)->ptk + 32)

static struct {
    uint8_t on, secure, chan, max, me[6];
    char ssid[33];
    uint8_t pmk[32], gtk[16];
    uint64_t gtk_pn;
    uint64_t next_beacon; uint16_t seq;
} a;
static sta_t st[NDF_WIFI_MAX_STA];
static uint32_t n_auth, n_assoc, n_refused, n_hs_timeout, n_beacons;
static struct { uint8_t mac[6]; uint32_t t; } pend[PEND_N];
#define data_rate WIFI_RATE_12M                 /* unicast data: DSSS frames over ~1200 B are not heard (docs/WIFI_MAC.md) */

static const uint8_t rsn_ie[22] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };

static uint16_t next_seq(void) { return a.seq++ & 0xFFF; }
static int tx(unsigned len) { return wifi_mac_tx(rf_wifi_tx_buffer(), len); }
static void zero(void *p, unsigned n) { volatile uint8_t *q = p; while (n--) *q++ = 0; }

static sta_t *find(const uint8_t *mac) { for (unsigned i = 0; i < NDF_WIFI_MAX_STA; i++) if (st[i].state && !memcmp(st[i].mac, mac, 6)) return &st[i]; return 0; }
static void drop(sta_t *s) { wifi_mac_clear_key((unsigned)(s - st)); zero(s, sizeof *s); }
static void deauth(const uint8_t *mac, uint16_t reason) { tx(wifi_build_deauth_ap(rf_wifi_tx_buffer(), mac, a.me, reason, next_seq())); }

/* --- transmit --- */
static int send_frame(const uint8_t dst[6], const uint8_t src[6], uint16_t ethertype, const uint8_t *p, unsigned n, const uint8_t *key, uint64_t *pn, uint8_t key_id, unsigned rate)
{
    uint8_t *fb = rf_wifi_tx_buffer();
    if (n > 1500) return -1;
    unsigned l = wifi_build_data_fromds(fb, a.me, dst, src, ethertype, p, n, next_seq());
    if (key) {                                               /* QoS data, CCMP: header 26 | CCMP header 8 | LLC/SNAP + payload | MIC 8 */
        memmove(fb + 26 + 8, fb + 24, l - 24); fb[0] = 0x88; fb[1] |= 0x40; fb[24] = fb[25] = 0;
        int c = ccmp_encrypt(key, fb, 26, ++*pn, key_id, fb + 26, l - 24); if (c < 0) return -1;
        l = 26 + (unsigned)c;
    }
    return wifi_mac_tx_rate(fb, l, rate);
}

int wifi_ap_send_eth(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n)
{
    if (!a.on) return -1;
    if (dst[0] & 1) return send_frame(dst, a.me, ethertype, p, n, a.secure ? a.gtk : 0, &a.gtk_pn, 1, WIFI_RATE_1M);   /* group traffic at the basic rate: every station hears it */
    sta_t *s = find(dst);
    if (!s || s->state < (a.secure ? UP : ASSOC)) return -1;
    return send_frame(dst, a.me, ethertype, p, n, a.secure ? TK(s) : 0, &s->tx_pn, 0, data_rate);
}

/* --- WPA2 authenticator --- */
static void key_msg(sta_t *s, uint16_t info, uint64_t replay, const uint8_t *data, unsigned dl)   /* EAPOL-Key from us, MIC when info says so */
{
    uint8_t e[4 + 95 + 64]; unsigned body = 95 + dl;
    memset(e, 0, sizeof e);
    e[0] = 2; e[1] = 3; e[2] = (uint8_t)(body >> 8); e[3] = (uint8_t)body; e[4] = 2; e[5] = (uint8_t)(info >> 8); e[6] = (uint8_t)info; e[8] = 16;
    for (int i = 0; i < 8; i++) e[9 + i] = (uint8_t)(replay >> (56 - 8 * i));
    memcpy(e + 17, s->anonce, 32);
    e[97] = (uint8_t)(dl >> 8); e[98] = (uint8_t)dl; if (dl) memcpy(e + 99, data, dl);
    if (info & EAPOL_INFO_MIC) { uint8_t m[20]; wpa_hmac_sha1(KCK(s), 16, e, 4 + body, m); memcpy(e + 81, m, 16); }
    send_frame(s->mac, a.me, 0x888E, e, 4 + body, 0, 0, 0, WIFI_RATE_1M);
}

static void msg1(sta_t *s) { s->replay++; s->t_msg = time_ms(); key_msg(s, 0x008a, s->replay, 0, 0); }

static void msg3(sta_t *s)
{
    uint8_t kd[48], w[56];
    memcpy(kd, rsn_ie, 22);
    kd[22] = 0xdd; kd[23] = 22; memcpy(kd + 24, "\x00\x0f\xac\x01", 4); kd[28] = 1; kd[29] = 0; memcpy(kd + 30, a.gtk, 16);   /* GTK KDE: key id 1 */
    kd[46] = 0xdd; kd[47] = 0;                                  /* padding to a multiple of 8 */
    aes_wrap(KEK(s), kd, sizeof kd, w);
    s->replay++; s->t_msg = time_ms();
    key_msg(s, 0x13ca, s->replay, w, sizeof w);
    zero(kd, sizeof kd);
}

static void eapol_rx(sta_t *s, const uint8_t *f, unsigned n)
{
    eapol_key_t e;
    if (!a.secure || s->state < HS2 || eapol_key_parse(f, n, &e) || (e.info & 7) != 2 || !(e.info & EAPOL_INFO_PAIRWISE) || (e.info & EAPOL_INFO_ACK)) return;
    if (e.replay != s->replay) return;                          /* answers to our latest message only */
    if (s->state == HS2 && (e.info & EAPOL_INFO_MIC)) {          /* message 2 */
        wpa_ptk(a.pmk, a.me, s->mac, s->anonce, e.nonce, s->ptk);
        if (eapol_key_check(KCK(s), f, n)) { zero(s->ptk, sizeof s->ptk); return; }          /* wrong passphrase: no key to answer with */
        wifi_mac_set_key((unsigned)(s - st), s->mac, TK(s), 0);                              /* the MAC decrypts this station's frames (key table slot per station) */
        s->state = HS4; s->tries = 0; msg3(s);
    } else if (s->state == HS4 && (e.info & EAPOL_INFO_MIC) && (e.info & EAPOL_INFO_SECURE)) {   /* message 4 */
        if (eapol_key_check(KCK(s), f, n)) return;
        s->state = UP; s->tx_pn = 0; memset(s->rx_pn, 0, sizeof s->rx_pn);
    }
}

/* --- receive --- */
static void to_station(const uint8_t *eth, unsigned n)       /* forward a frame between two stations */
{
    sta_t *d = find(eth);
    if (d && d->state >= (a.secure ? UP : ASSOC)) send_frame(eth, eth + 6, (uint16_t)(eth[12] << 8 | eth[13]), eth + 14, n - 14, a.secure ? TK(d) : 0, &d->tx_pn, 0, data_rate);
}

static void data_rx(const wifi_rx_t *r)
{
    const uint8_t *f = r->data; unsigned len = r->len;
    if ((f[1] & 3) != 1 || memcmp(f + 4, a.me, 6) || len > 1600) return;
    sta_t *s = find(f + 10);
    if (!s) { deauth(f + 10, 7); return; }                       /* class 3 frame from a station that is not associated */
    s->last_rx = time_ms();
    if (f[0] == 0x48 || f[0] == 0xC8) return;                    /* null / QoS null: keep-alive */
    uint16_t sq = (uint16_t)((f[22] | f[23] << 8) >> 4);
    if ((f[1] & 0x08) && sq == s->last_seq) return;              /* retransmission of a frame already handled */
    s->last_seq = sq;
    static uint8_t eth[1600];
    uint8_t *pl = (uint8_t *)f; unsigned hl = 24 + ((f[0] & 0x80) ? 2 : 0), prot = f[1] & 0x40;
    if (prot) {                                                  /* CCMP, decrypted and MIC-checked by the MAC: header kept, MIC gone */
        if (s->state != UP || !r->hw_decrypted || len < hl + 8) return;
        unsigned tid = (f[0] & 0x80) ? f[24] & 7 : 0;
        uint64_t pn = (uint64_t)pl[hl] | (uint64_t)pl[hl + 1] << 8 | (uint64_t)pl[hl + 4] << 16 | (uint64_t)pl[hl + 5] << 24 | (uint64_t)pl[hl + 6] << 32 | (uint64_t)pl[hl + 7] << 40;
        if (pn <= s->rx_pn[tid]) return;                         /* replayed packet number */
        s->rx_pn[tid] = pn;
        memmove(pl + hl, pl + hl + 8, len - hl - 8); pl[1] &= (uint8_t)~0x40; len -= 8;
    } else if (a.secure && s->state < HS2) return;
    int n = wifi_ap_data_to_eth(pl, len, a.me, eth, sizeof eth);
    if (n < 14) return;
    uint16_t et = (uint16_t)(eth[12] << 8 | eth[13]);
    if (et == 0x888E) { eapol_rx(s, eth + 14, (unsigned)n - 14); return; }
    if (a.secure && !prot) return;                               /* on a protected network only EAPOL may arrive in the clear */
    if (a.secure && s->state != UP) return;
    if (memcmp(eth, a.me, 6) && !(eth[0] & 1)) { to_station(eth, (unsigned)n); return; }
    if (wifi_eth_rx) wifi_eth_rx(eth, (unsigned)n);
}

static void mgmt_rx(const wifi_rx_t *r)
{
    const uint8_t *f = r->data; unsigned len = r->len; uint8_t *fb = rf_wifi_tx_buffer(); uint32_t now = time_ms();
    uint8_t bc = f[4] == 0xFF, mine = !memcmp(f + 4, a.me, 6);
    switch (f[0]) {
    case 0x40: {                                                 /* probe request: wildcard or our SSID */
        const uint8_t *v; int l = wifi_ie_find(f + 24, len - 24, 0, &v);
        if ((!bc && !mine) || l < 0 || (l && ((unsigned)l != strlen(a.ssid) || memcmp(v, a.ssid, (unsigned)l)))) return;
        tx(wifi_build_beacon(fb, f + 10, a.me, a.ssid, a.chan, a.secure, time_us(), next_seq(), 1));
        return; }
    case 0xB0: {                                                 /* authentication */
        if (!mine || len < 30) return;
        uint16_t algo = (uint16_t)(f[24] | f[25] << 8), seq = (uint16_t)(f[26] | f[27] << 8), status = 0;
        if (algo != 0) status = 13; else if (seq != 1) status = 14;
        if (!status) {                                           /* remember the asker; the oldest entry goes when the list is full (junk cannot pin us) */
            unsigned k = 0; for (unsigned i = 0; i < PEND_N; i++) { if (!memcmp(pend[i].mac, f + 10, 6)) { k = i; break; } if (pend[i].t < pend[k].t) k = i; }
            memcpy(pend[k].mac, f + 10, 6); pend[k].t = now ? now : 1;
        }
        n_auth++;
        tx(wifi_build_auth_resp(fb, f + 10, a.me, status, next_seq()));
        return; }
    case 0x00: {                                                 /* association request */
        if (!mine || len < 28) return;
        unsigned ok = 0; for (unsigned i = 0; i < PEND_N; i++) if (!memcmp(pend[i].mac, f + 10, 6) && pend[i].t && now - pend[i].t < AUTH_MS) ok = 1;
        if (!ok && !find(f + 10)) { deauth(f + 10, 6); return; }   /* class 2 frame from a station that did not authenticate */
        const uint8_t *v; int l = wifi_ie_find(f + 28, len - 28, 0, &v);
        uint16_t status = 0;
        if (l < 0 || (unsigned)l != strlen(a.ssid) || memcmp(v, a.ssid, (unsigned)l)) status = 1;
        else if (a.secure && wifi_ie_find(f + 28, len - 28, 48, &v) < 0) status = 40;      /* no RSN element on a protected network */
        sta_t *s = find(f + 10);
        if (!status && !s) {
            unsigned used = 0; for (unsigned i = 0; i < NDF_WIFI_MAX_STA; i++) used += st[i].state != FREE;
            if (used >= a.max) status = 17;                      /* AP cannot handle another station */
            else for (unsigned i = 0; i < NDF_WIFI_MAX_STA; i++) if (!st[i].state) { s = &st[i]; break; }
        }
        if (!status) {
            if (s->state == FREE || s->state == UP || s->state == HS4 || s->state == HS2) { drop(s); memcpy(s->mac, f + 10, 6); }   /* a repeated association restarts the station */
            s->aid = (uint16_t)(s - st + 1); s->state = ASSOC; s->t0 = s->last_rx = now; s->last_seq = 0xFFFF;
        }
        if (status) n_refused++; else n_assoc++;
        tx(wifi_build_assoc_resp(fb, f + 10, a.me, status, s ? s->aid : 0, a.secure, next_seq()));
        if (!status && a.secure) { rng_fill(s->anonce, 32); s->state = HS2; s->replay = 0; s->tries = 0; msg1(s); }
        return; }
    case 0xC0: case 0xA0: {                                      /* deauthentication, disassociation */
        sta_t *s = mine ? find(f + 10) : 0;
        if (s) drop(s);
        return; }
    default: return;
    }
}

static void on_frame(const wifi_rx_t *r)
{
    if (r->len < 24) return;
    if ((r->data[0] & 0x0C) == 0x08) data_rx(r);
    else if ((r->data[0] & 0x0C) == 0x00) mgmt_rx(r);
}

/* --- control --- */
int wifi_ap_start(const char *ssid, const char *pass, uint8_t chan, unsigned max_sta)
{
    if (!ssid || !*ssid || strlen(ssid) > 32 || chan < 1 || chan > 13 || !max_sta) return -1;
    if (pass && (strlen(pass) < 8 || strlen(pass) > 63)) return -1;
    memset(&a, 0, sizeof a); memset(st, 0, sizeof st); memset(pend, 0, sizeof pend);
    memcpy(a.me, wifi_mac_addr(), 6); strcpy(a.ssid, ssid); a.chan = chan; a.max = (uint8_t)(max_sta > NDF_WIFI_MAX_STA ? NDF_WIFI_MAX_STA : max_sta);
    if (pass) { a.secure = 1; wpa_pmk(pass, (const uint8_t *)ssid, (unsigned)strlen(ssid), a.pmk); rng_fill(a.gtk, 16); }
    if (wifi_mac_set_channel(chan)) return -1;
    a.on = 1; a.next_beacon = time_us();
    return 0;
}

void wifi_ap_stop(void)
{
    if (!a.on) return;
    for (unsigned i = 0; i < NDF_WIFI_MAX_STA; i++) if (st[i].state) deauth(st[i].mac, 3);   /* station leaves */
    wifi_mac_clear_keys(); zero(&a, sizeof a); zero(st, sizeof st);
}

unsigned wifi_ap_stations(uint8_t macs[][6], unsigned max)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NDF_WIFI_MAX_STA && n < max; i++) if (st[i].state >= (a.secure ? UP : ASSOC)) memcpy(macs[n++], st[i].mac, 6);
    return n;
}

void wifi_ap_stats(uint32_t *auth, uint32_t *assoc, uint32_t *refused, uint32_t *hs_timeout, uint32_t *beacons, unsigned *used)
{
    if (auth) *auth = n_auth;
    if (assoc) *assoc = n_assoc;
    if (refused) *refused = n_refused;
    if (hs_timeout) *hs_timeout = n_hs_timeout;
    if (beacons) *beacons = n_beacons;
    if (used) { *used = 0; for (unsigned i = 0; i < NDF_WIFI_MAX_STA; i++) *used += st[i].state != FREE; }
}

void wifi_ap_poll(void)
{
    if (!a.on) return;
    wifi_rx_t r;
    while (wifi_mac_rx(&r, 0)) on_frame(&r);
    uint32_t now = time_ms();
    uint64_t t = time_us();
    if (t >= a.next_beacon) {                                    /* the next beacon is due at a fixed grid: a late one does not move the others */
        tx(wifi_build_beacon(rf_wifi_tx_buffer(), (const uint8_t *)"\xff\xff\xff\xff\xff\xff", a.me, a.ssid, a.chan, a.secure, t, next_seq(), 0));
        n_beacons++; a.next_beacon += BEACON_US; if (t >= a.next_beacon) a.next_beacon = t + BEACON_US;
    }
    for (unsigned i = 0; i < NDF_WIFI_MAX_STA; i++) {
        sta_t *s = &st[i];
        if (!s->state) continue;
        if ((s->state == HS2 || s->state == HS4) && now - s->t0 > HS_TIMEOUT_MS) { n_hs_timeout++; deauth(s->mac, 15); drop(s); continue; }
        if (s->state == ASSOC && a.secure && now - s->t0 > HS_TIMEOUT_MS) { deauth(s->mac, 15); drop(s); continue; }
        if ((s->state == HS2 || s->state == HS4) && now - s->t_msg > HS_RETRY_MS) { if (s->state == HS2) msg1(s); else msg3(s); }
        if (s->state == UP && now - s->last_rx > IDLE_MS) { deauth(s->mac, 4); drop(s); }
        else if (s->state == ASSOC && !a.secure && now - s->last_rx > IDLE_MS) { deauth(s->mac, 4); drop(s); }
    }
}
