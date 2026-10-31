/* Station: scan, join (open, WPA2-PSK / PSK-SHA256 with PMF, CCMP), data in and out. Driven by wifi_poll() from the main loop; scan and join pump the radio themselves. */
#include <string.h>
#include "wifi.h"
#include "wifi_mac.h"
#include "wpa.h"
#include "sae.h"
#include "rng.h"
#include "sys.h"

#define KEEPALIVE_MS 20000u                 /* the AP drops silent stations after minutes (IDF: 300 s) */

void (*wifi_eth_rx)(const uint8_t *frame, unsigned n);

enum { DOWN, AUTHING, ASSOCING, HANDSHAKE, UP };
static struct {
    uint8_t me[6], bssid[6], chan, state; uint16_t seq, beacon_tu, rx_sc[9];   /* rx_sc: last sequence control (sequence | fragment) received per TID 0..7, [8] non-QoS: the duplicate cache (802.11-2020 10.3.2.14) */
    uint32_t last_beacon, last_tx;
    wifi_ap_t *out; unsigned max, n; const char *want;       /* scan collection; want = only this SSID (join) */
    uint8_t secure;                                          /* WPA2 network */
    uint8_t akm, ver, pmf;                                   /* AKM suite (2 PSK, 6 PSK-SHA256, 8 SAE), EAPOL-Key descriptor version (2 HMAC-SHA1, 3 AES-CMAC, 0 AKM-defined = CMAC for SAE), management frame protection on */
    uint8_t sae_st, token[64], token_len, token_new;        /* SAE: 0 commit sent, 1 confirm sent, 2 confirmed, 3 failed; anti-clogging token from the AP */
    uint8_t rsn[2 + 26 + 18];                                     /* the RSN element we send (association request, message 2) */
    uint32_t comeback_ms;                                    /* association refused with status 30: retry after this */
} s;
static struct {                                              /* WPA2 state; wiped when the link goes down */
    uint8_t pmk[32], ptk[48], anonce[32], snonce[32], gtk[16], gtk_id, igtk[16];
    uint64_t replay, tx_pn, rx_pn[2][8], mgmt_pn, ipn;       /* last EAPOL replay counter; CCMP packet numbers: transmit, receive [group][TID], protected unicast management; BIP IPN */
    uint8_t have_ptk, keys, have_gtk, have_igtk, have_replay, msg1_seen;
} k;
#define KCK (k.ptk)
#define KEK (k.ptk + 16)
#define TK  (k.ptk + 32)
static uint8_t fb[1600];
#ifndef NDF_WIFI_DATA_RATE
#define NDF_WIFI_DATA_RATE 0xD                /* 36 Mbit/s OFDM: the fastest rate the peer reliably hears from us (48/54 are not, docs/WIFI_MAC.md) */
#endif
static unsigned data_rate = NDF_WIFI_DATA_RATE;
void wifi_set_data_rate(unsigned rate) { data_rate = rate; }

static int tx(unsigned len) { s.last_tx = time_ms(); return wifi_mac_tx(fb, len); }
static uint16_t next_seq(void) { return s.seq++ & 0xFFF; }

static void zero(void *p, unsigned n) { volatile uint8_t *q = p; while (n--) *q++ = 0; }
static void down(void) { wifi_mac_clear_keys(); s.state = DOWN; zero(&k.ptk, sizeof k.ptk); zero(&k.gtk, sizeof k.gtk); zero(&k.igtk, sizeof k.igtk); memset(k.rx_pn, 0, sizeof k.rx_pn); k.tx_pn = k.mgmt_pn = k.ipn = 0; k.have_ptk = k.keys = k.have_gtk = k.have_igtk = k.have_replay = k.msg1_seen = 0; }

static int send_eth_rate(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n, int encrypt, unsigned rate);
static int send_eth(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n, int encrypt) { return send_eth_rate(dst, ethertype, p, n, encrypt, data_rate); }
static int send_eth_rate(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n, int encrypt, unsigned rate)
{
    unsigned l = wifi_build_data(fb, s.me, s.bssid, dst, ethertype, p, n, next_seq());
    if (encrypt) {                                           /* QoS data (as the IDF station sends): header 26 | CCMP header 8 | LLC/SNAP + payload | MIC 8 */
        memmove(fb + 26 + 8, fb + 24, l - 24); fb[0] = 0x88; fb[1] |= 0x40; fb[24] = fb[25] = 0;
        uint64_t pn = ++k.tx_pn; uint8_t *h = fb + 26;                /* CCMP header in software, encryption and MIC in the MAC (IDF's path) */
        h[0] = (uint8_t)pn; h[1] = (uint8_t)(pn >> 8); h[2] = 0; h[3] = 0x20; for (int i = 0; i < 4; i++) h[4 + i] = (uint8_t)(pn >> (16 + 8 * i));
        l = 26 + 8 + (l - 24) + 8;
        s.last_tx = time_ms();
        return wifi_mac_tx_hwcrypt(fb, l, rate);
    }
    s.last_tx = time_ms();
    return wifi_mac_tx_rate(fb, l, rate);               /* data at 12 Mbit/s: DSSS frames over ~1200 B are not heard (docs/WIFI_MAC.md) */
}

/* --- WPA2 4-way and group key handshake (station side) --- */
/* RSN element as IDF's supplicant writes it (wpa_gen_wpa_ie_rsn): CCMP group and pairwise, one AKM, capabilities MFPC (PMF capable, IDF's default) and
 * MFPR for SAE; PMKID count 0 and the management group cipher (BIP-CMAC-128) only when the AP offers PMF. */
static void rsn_build(uint8_t akm, int pmf, const uint8_t *pmkid)   /* pmkid: after SAE (IDF's cur_pmksa), else NULL */
{
    static const uint8_t head[] = { 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac };
    uint8_t *o = s.rsn; unsigned n = 2;
    memcpy(o + n, head, sizeof head); n += sizeof head; o[n++] = akm;
    o[n++] = (uint8_t)(0x80 | (akm == 8 ? 0x40 : 0)); o[n++] = 0;
    if (pmkid) { o[n++] = 1; o[n++] = 0; memcpy(o + n, pmkid, 16); n += 16; }
    if (pmf) { if (!pmkid) { o[n++] = 0; o[n++] = 0; } o[n++] = 0x00; o[n++] = 0x0f; o[n++] = 0xac; o[n++] = 6; }
    o[0] = 48; o[1] = (uint8_t)(n - 2);
}
static void mic(const uint8_t *e, unsigned n, uint8_t out[16]) { uint8_t m[20]; if (s.ver == 2) wpa_hmac_sha1(KCK, 16, e, n, m); else wpa_cmac(KCK, e, n, m); memcpy(out, m, 16); }
static int mic_ok(const uint8_t *f, unsigned n) { return s.ver == 2 ? !eapol_key_check(KCK, f, n) : !eapol_key_check_cmac(KCK, f, n); }

static void hs_send(uint16_t info, uint64_t replay, const uint8_t *nonce, const uint8_t *data, unsigned dl)
{
    uint8_t e[4 + 95 + sizeof s.rsn]; unsigned body = 95 + dl;
    if (dl > sizeof s.rsn) return;
    memset(e, 0, sizeof e);
    info = (uint16_t)((info & ~7u) | s.ver);
    e[0] = 2; e[1] = 3; e[2] = (uint8_t)(body >> 8); e[3] = (uint8_t)body; e[4] = 2; e[5] = (uint8_t)(info >> 8); e[6] = (uint8_t)info;
    for (int i = 0; i < 8; i++) e[9 + i] = (uint8_t)(replay >> (56 - 8 * i));
    if (nonce) memcpy(e + 17, nonce, 32);
    e[97] = (uint8_t)(dl >> 8); e[98] = (uint8_t)dl; if (dl) memcpy(e + 99, data, dl);
    mic(e, 4 + body, e + 81);
    send_eth_rate(s.bssid, 0x888E, e, 4 + body, 0, WIFI_RATE_1M);       /* the handshake at 1 Mbit/s: the AP misses 12 Mbit/s frames now and then (measured: 6 of 6 joins instead of about half) */
}

static int take_gtk(const uint8_t *d, unsigned n)                /* GTK KDE (and with PMF the IGTK KDE) inside the unwrapped key data; 0 = found and stored */
{
    int gtk = 0, igtk = 0;
    for (unsigned i = 0; i + 2 <= n; ) {
        unsigned l = d[i + 1];
        if (i + 2 + l > n) return -1;
        if (d[i] == 0xdd && l >= 6 + 16 && !memcmp(d + i + 2, "\x00\x0f\xac\x01", 4)) { memcpy(k.gtk, d + i + 8, 16); k.gtk_id = d[i + 6] & 3; k.have_gtk = gtk = 1; memset(k.rx_pn[1], 0, sizeof k.rx_pn[1]); }
        if (d[i] == 0xdd && l >= 4 + 8 + 16 && !memcmp(d + i + 2, "\x00\x0f\xac\x09", 4)) {      /* IGTK KDE: key id 2, IPN 6, IGTK 16 */
            memcpy(k.igtk, d + i + 14, 16); k.ipn = 0; for (int j = 5; j >= 0; j--) k.ipn = k.ipn << 8 | d[i + 8 + j]; k.have_igtk = igtk = 1;
        }
        if (d[i] == 0xdd && l == 0) break;                                                /* padding */
        i += 2 + l;
    }
    return gtk && (igtk || !s.pmf) ? 0 : -1;
}

static void eapol_rx(const uint8_t *f, unsigned n)
{
    eapol_key_t e;
    if (!s.secure || eapol_key_parse(f, n, &e) || (e.info & 7) != s.ver) return;
    if ((e.info & EAPOL_INFO_PAIRWISE) && (e.info & EAPOL_INFO_ACK) && !(e.info & EAPOL_INFO_MIC)) {          /* message 1 */
        if (s.state != HANDSHAKE) return;
        if (++k.msg1_seen > 2 && k.have_ptk) { down(); return; }                      /* the AP has asked three times: our message 2 is being refused (wrong passphrase) */
        memcpy(k.anonce, e.nonce, 32); rng_fill(k.snonce, 32);
        if (s.akm == 2) wpa_ptk(k.pmk, s.bssid, s.me, k.anonce, k.snonce, k.ptk); else wpa_ptk_sha256(k.pmk, s.bssid, s.me, k.anonce, k.snonce, k.ptk);
        k.have_ptk = 1;
        k.replay = e.replay; k.have_replay = 1;
        hs_send(0x010a, e.replay, k.snonce, s.rsn, 2u + s.rsn[1]);
        return;
    }
    if (!k.have_ptk || !mic_ok(f, n)) return;                           /* messages 3 and the group key need a good MIC */
    if (k.have_replay && e.replay <= k.replay && !(k.keys && e.replay == k.replay && (e.info & EAPOL_INFO_PAIRWISE))) return;   /* replayed (a retransmitted message 3 after install is answered below) */
    if (!(e.info & EAPOL_INFO_ACK) || !(e.info & EAPOL_INFO_SECURE) || !(e.info & EAPOL_INFO_ENCDATA)) return;
    if (e.info & EAPOL_INFO_PAIRWISE) {                                               /* message 3 */
        if (!(e.info & EAPOL_INFO_INSTALL) || memcmp(e.nonce, k.anonce, 32)) return;  /* a different ANonce than message 1 is an attack or a restart */
        if (k.keys) { hs_send(0x030a, e.replay, 0, 0, 0); return; }                  /* retransmitted message 3: message 4 was lost */
        uint8_t kd[WPA_UNWRAP_MAX]; int kl = aes_unwrap(KEK, e.data, e.data_len, kd);
        if (kl < 0 || take_gtk(kd, (unsigned)kl)) { zero(kd, sizeof kd); return; }    /* nothing installed on a bad message 3 */
        k.replay = e.replay; hs_send(0x030a, e.replay, 0, 0, 0);
        wifi_mac_set_key(0, s.bssid, TK, 0); wifi_mac_set_key(1, s.bssid, k.gtk, k.gtk_id);
        k.keys = 1; k.tx_pn = 0; memset(k.rx_pn, 0, sizeof k.rx_pn); s.state = UP; s.last_beacon = time_ms();
        zero(kd, sizeof kd);
    } else if (s.state == UP) {                                                       /* group key update */
        uint8_t kd[WPA_UNWRAP_MAX]; int kl = aes_unwrap(KEK, e.data, e.data_len, kd);
        if (kl < 0 || take_gtk(kd, (unsigned)kl)) { zero(kd, sizeof kd); return; }
        wifi_mac_set_key(1, s.bssid, k.gtk, k.gtk_id);
        k.replay = e.replay; hs_send(0x0302, e.replay, 0, 0, 0);
        zero(kd, sizeof kd);
    }
}

/* PMF (802.11w) receive check for deauthentication / disassociation / action frames. Unicast: CCMP with the TK (decrypted in place, by us or by the
 * MAC's key table), packet number above the last one. Group-addressed: BIP-CMAC-128 MMIE with the IGTK, IPN above the last one. 1 = accept. */
static int mgmt_ok(const wifi_rx_t *r)
{
    uint8_t *f = (uint8_t *)r->data; unsigned len = r->len; uint64_t pn;
    if (f[4] & 1) {
        if (!k.have_igtk || f[1] & 0x40 || wpa_bip_check(k.igtk, f, len, &pn) || pn <= k.ipn) return 0;
        k.ipn = pn; return 1;
    }
    if (!(f[1] & 0x40) || len < 24 + 8 + (r->hw_decrypted ? 0 : 8)) return 0;   /* CCMP header, and the MIC unless the MAC has removed it */
    int d;
    if (r->hw_decrypted) { pn = (uint64_t)f[24] | (uint64_t)f[25] << 8 | (uint64_t)f[28] << 16 | (uint64_t)f[29] << 24 | (uint64_t)f[30] << 32 | (uint64_t)f[31] << 40; d = (int)len - 24 - 8; }
    else d = ccmp_decrypt(TK, f, 24, f + 24, len - 24, &pn);
    if (d < 0 || pn <= k.mgmt_pn) return 0;
    k.mgmt_pn = pn; memmove(f + 24, f + 24 + 8, (unsigned)d); f[1] &= (uint8_t)~0x40;        /* plaintext body back at 24 */
    return 1;
}

/* SA Query response (802.11w 11.13): the transaction id echoed, protected with the TK. Software CCMP; the MAC sends it as is. */
static void mgmt_tx(uint8_t fc0, const uint8_t *body, unsigned bl, int protect)   /* a management frame to the AP; protected: software CCMP with the TK */
{
    unsigned n = wifi_build_deauth(fb, s.me, s.bssid, 0, next_seq()) - 2;                       /* a management header to the AP */
    fb[0] = fc0; fb[1] = protect ? 0x40 : 0;
    int l = (int)bl;
    if (protect) { memcpy(fb + n + 8, body, bl); l = ccmp_encrypt(TK, fb, 24, ++k.tx_pn, 0, fb + n, bl); }
    else memcpy(fb + n, body, bl);
    if (l > 0) { s.last_tx = time_ms(); wifi_mac_tx_rate(fb, n + (unsigned)l, WIFI_RATE_1M); }
}
static void sa_query_reply(const uint8_t *tid) { uint8_t b[4] = { 8, 1, tid[0], tid[1] }; mgmt_tx(0xD0, b, 4, 1); }
static void pump(uint32_t ms);
static void leave(void)                                      /* deauthenticate (reason 3, leaving) before dropping an up link, as IDF does: a PMF AP would otherwise keep us */
{
    static const uint8_t reason[2] = { 3, 0 };
    if (s.state == UP) { mgmt_tx(0xC0, reason, 2, s.pmf && k.keys); pump(5); }
    down();
}

/* --- SAE authentication (algorithm 3) --- */
static sae_t sae;
static void sae_tx(uint8_t seq, const uint8_t *b, unsigned bl)
{
    unsigned n = wifi_build_auth(fb, s.me, s.bssid, next_seq()) - 6;
    fb[n] = 3; fb[n + 1] = 0; fb[n + 2] = seq; fb[n + 3] = 0; fb[n + 4] = fb[n + 5] = 0; memcpy(fb + n + 6, b, bl);
    tx(n + 6 + bl);
}
static void sae_commit_tx(void) { uint8_t b[2 + 64 + 96]; sae_tx(1, b, sae_write_commit(&sae, b, s.token, s.token_len)); }
static void sae_rx(const uint8_t *f, unsigned len)
{
    unsigned seq = f[26] | f[27] << 8, st = f[28] | f[29] << 8; const uint8_t *b = f + 30; unsigned bl = len - 30;
    if (f[24] != 3 || f[25]) return;
    if (seq == 1 && st == 76 && bl > 2 && bl - 2 <= sizeof s.token) { memcpy(s.token, b + 2, bl - 2); s.token_len = (uint8_t)(bl - 2); s.token_new = 1; return; }   /* anti-clogging: commit again with the token */
    if (seq == 1 && st == 0 && s.sae_st <= 1) {
        if (s.sae_st == 1 && !memcmp(b + 2, sae.peer_scalar, 32)) { uint8_t c[34]; sae_tx(2, c, sae_write_confirm(&sae, c)); return; }   /* the AP repeated its commit: our confirm was lost */
        if (sae_process_commit(&sae, b, bl)) { s.sae_st = 3; return; }
        uint8_t c[34]; sae_tx(2, c, sae_write_confirm(&sae, c)); s.sae_st = 1;
        return;
    }
    if (seq == 2 && st == 0 && s.sae_st == 1) { if (sae_check_confirm(&sae, b, bl)) { s.sae_st = 3; return; } s.sae_st = 2; s.state = ASSOCING; }
    else if (st) s.sae_st = 3;                               /* refused (unknown group, H2E only, ...) */
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
    case 0xB0:                                               /* authentication: ours, transaction 2, success; SAE: the AP's commit and confirm */
        if (s.state != AUTHING || len < 30 || memcmp(f + 10, s.bssid, 6) || memcmp(f + 4, s.me, 6)) return;
        if (s.akm == 8) { sae_rx(f, len); return; }
        if (f[24] == 0 && f[26] == 2 && !f[28] && !f[29]) s.state = ASSOCING;
        return;
    case 0x10:                                               /* association response: success, or 30 (PMF: try again after the comeback time) */
        if (s.state != ASSOCING || len < 30 || memcmp(f + 10, s.bssid, 6) || memcmp(f + 4, s.me, 6)) return;
        if (!f[26] && !f[27]) { s.state = s.secure ? HANDSHAKE : UP; s.last_beacon = time_ms(); k.msg1_seen = 0; }
        else if (f[26] == 30 && !f[27]) {                    /* Timeout Interval element (56): type 3 = association comeback time in TU */
            const uint8_t *v; int l = wifi_ie_find(f + 30, len - 30, 56, &v);
            uint32_t tu = l >= 5 && v[0] == 3 ? (uint32_t)v[1] | v[2] << 8 | v[3] << 16 | (uint32_t)v[4] << 24 : 1000;
            s.comeback_ms = tu > 5000 ? 5000 : tu * 1024 / 1000 + 1;
        }
        return;
    case 0xC0: case 0xA0:                                    /* deauthentication, disassociation: only from our AP */
        if (s.state == DOWN || memcmp(f + 10, s.bssid, 6) || memcmp(f + 16, s.bssid, 6)) return;
        if (s.pmf && k.keys && !mgmt_ok(r)) return;          /* PMF: an unprotected or forged one is dropped (IDF drops it too) */
        down();
        return;
    case 0xD0:                                               /* action: SA Query request (category 8, action 0) from our AP, protected */
        if (!s.pmf || !k.keys || memcmp(f + 10, s.bssid, 6) || !mgmt_ok(r)) return;
        if (len >= 24 + 4 && f[24] == 8 && f[25] == 0) sa_query_reply(f + 26);
        return;
    default: break;
    }
    if (s.state < HANDSHAKE || (f[0] & 0x0C) != 0x08) return;
    { unsigned tid = (f[0] & 0x80) ? f[24] & 7 : 8; uint16_t sc = (uint16_t)(f[22] | f[23] << 8);   /* a retry with the sequence control we already have for that TID: the AP missed our ACK */
      if ((f[1] & 0x08) && sc == s.rx_sc[tid]) return;
      s.rx_sc[tid] = sc; }
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
static unsigned b_assoc(void) { return wifi_build_assoc_req(fb, s.me, s.bssid, join_ssid, s.secure ? s.rsn : 0, next_seq()); }

int wifi_sta_join(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    uint32_t t0 = time_ms();
    wifi_ap_t found[8]; unsigned n = 0;
    leave();
    scan(found, 8, &n, ssid);
    unsigned i = 0; while (i < n && strcmp(found[i].ssid, ssid)) i++;
    if (i == n) return -1;
    if (pass ? found[i].sec != 2 : found[i].sec) return -2;  /* open join to a protected network, or a passphrase for an open / non-WPA2 one */
    s.secure = pass != 0;
    if (pass) {                                              /* AKM as IDF picks it: SAE, then PSK-SHA256, then PSK; PMF when the AP is capable (always with SAE) */
        uint16_t a = found[i].akm ? found[i].akm : 1u << 2;
        s.akm = (a & 1u << 8) ? 8 : (a & 1u << 6) ? 6 : (a & 1u << 2) ? 2 : 0;
        if (!s.akm) return -2;
        s.ver = s.akm == 2 ? 2 : s.akm == 6 ? 3 : 0; s.pmf = (found[i].rsn_caps & 0x80) != 0;
        if (((found[i].rsn_caps & 0x40) || s.akm == 8) && !s.pmf) return -2;
        if (s.akm != 8) { rsn_build(s.akm, s.pmf, 0); wpa_pmk(pass, (const uint8_t *)ssid, (unsigned)strlen(ssid), k.pmk); }
    }
    memcpy(s.bssid, found[i].bssid, 6); s.beacon_tu = found[i].beacon_tu ? found[i].beacon_tu : 100;
    wifi_mac_set_channel(found[i].chan); s.chan = found[i].chan;
    join_ssid = ssid; s.state = AUTHING;
    if (s.secure && s.akm == 8) {                            /* SAE: commit, the AP's commit, confirm, the AP's confirm (up to 3 commits, anti-clogging token honoured) */
        if (sae_prepare(&sae, s.me, s.bssid, pass, (unsigned)strlen(pass))) { down(); return -1; }
        s.sae_st = 0; s.token_len = 0;
        for (int t = 0; t < 3 && s.state == AUTHING && s.sae_st != 3; t++) {
            s.token_new = 0; sae_commit_tx();
            uint32_t w0 = time_ms();
            while (time_ms() - w0 < 400 && s.state == AUTHING && s.sae_st != 3 && !s.token_new) pump(2);
            if (s.token_new) t--;                            /* a token request does not use up a try */
        }
        if (s.state != ASSOCING) { sae_wipe(&sae); down(); return s.sae_st == 3 ? -3 : -1; }   /* a wrong password ends in a failed confirm */
        memcpy(k.pmk, sae.pmk, 32); rsn_build(8, 1, sae.pmkid); sae_wipe(&sae);
    } else if (step(ASSOCING, b_auth, 100) || time_ms() - t0 > timeout_ms) { down(); return -1; }
    s.comeback_ms = 0;
    int rc = step(s.secure ? HANDSHAKE : UP, b_assoc, 100);
    if (rc && s.comeback_ms) {                               /* refused for now (the AP still holds an old PMF association and checks it with an SA Query) */
        uint32_t w = s.comeback_ms; s.comeback_ms = 0; pump(w); rc = step(s.secure ? HANDSHAKE : UP, b_assoc, 100);
    }
    if (rc) { down(); return -1; }
    memset(s.rx_sc, 0xFF, sizeof s.rx_sc);
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
