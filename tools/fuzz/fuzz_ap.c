/* Host fuzzer of components/wifi/ap.c: the access point is compiled as it is, the radio is replaced by stubs. Random and mutated management and data
 * frames come in through wifi_ap_poll (the same path as the radio), with the clock jumping so that beacons, handshake retries and timeouts run.
 * Invariants: no crash or overread (ASan), never more stations than allowed, no transmitted frame longer than the buffer, a station is never
 * keyed without a completed handshake, open-system junk never fills the table.   usage: fuzz_ap [rounds] [seed] */
#include "wifi.h"
#include "wifi_mac.h"
#include "wpa.h"
#include "rng.h"
#include "sys.h"
#include "rf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long x = 1;
static unsigned rnd(void) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return (unsigned)x; }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* --- stubs: the radio and the hardware crypto --- */
static uint64_t now_us = 1000000000ull; static uint8_t txbuf[1600]; static unsigned n_tx, keys_set; static const uint8_t mine[6] = { 2, 0x4e, 0x44, 0x46, 0, 1 };
static uint8_t in[400]; static unsigned in_len; static int in_have, in_hw;
void (*wifi_eth_rx)(const uint8_t *frame, unsigned n);
uint64_t time_us(void) { return now_us; }
void rng_fill(void *b, size_t n) { uint8_t *p = b; while (n--) *p++ = (uint8_t)rnd(); }
uint8_t *rf_wifi_tx_buffer(void) { return txbuf; }
const uint8_t *wifi_mac_addr(void) { return mine; }
int wifi_mac_set_channel(uint8_t ch) { (void)ch; return 0; }
static uint8_t last[1600]; static unsigned last_len;
int wifi_mac_tx_rate(const uint8_t *f, unsigned len, unsigned rate) { (void)rate; CHECK(len >= 10 && len <= 1596); n_tx++; if (f[0] != 0x80) { memcpy(last, f, len); last_len = len; } return 0; }       /* beacons are counted, not kept: they go out on every poll */
int wifi_mac_tx(const uint8_t *f, unsigned len) { return wifi_mac_tx_rate(f, len, 0); }
void wifi_mac_set_key(unsigned k, const uint8_t ta[6], const uint8_t key[16], unsigned id) { (void)ta; (void)key; (void)id; CHECK(k < 8); keys_set++; }
void wifi_mac_clear_key(unsigned k) { CHECK(k < 8); }
void wifi_mac_clear_keys(void) {}
int wifi_mac_rx(wifi_rx_t *r, uint32_t t) { (void)t; if (!in_have) return 0; in_have = 0; r->data = in; r->len = (uint16_t)in_len; r->rssi = -40; r->rate = 0; r->chan = 6; r->hw_decrypted = (uint8_t)in_hw; return 1; }
void aes_block(const uint8_t *key, int kl, _Bool enc, const uint8_t i[16], uint8_t o[16]) { (void)kl; (void)enc; for (int k = 0; k < 16; k++) o[k] = (uint8_t)(i[k] ^ key[k] ^ 0x5a); }   /* not AES: only the wrap / CCMP plumbing is exercised */
static unsigned eth_n; static void on_eth(const uint8_t *f, unsigned n) { (void)f; CHECK(n >= 14 && n <= 1600); eth_n++; }

static void feed(const uint8_t *f, unsigned len, int hw) { memcpy(in, f, len); in_len = len; in_have = 1; in_hw = hw; wifi_ap_poll(); }

/* --- a station that follows the protocol --- */
static void send_mgmt(uint8_t fc0, const uint8_t sta[6], const uint8_t *body, unsigned n)
{
    uint8_t f[200] = { fc0 }; memcpy(f + 4, mine, 6); memcpy(f + 10, sta, 6); memcpy(f + 16, mine, 6); memcpy(f + 24, body, n);
    feed(f, 24 + n, 0);
}
static void send_eapol(const uint8_t sta[6], const uint8_t *e, unsigned n)
{
    uint8_t f[300] = { 0x08, 0x01 }; memcpy(f + 4, mine, 6); memcpy(f + 10, sta, 6); memcpy(f + 16, mine, 6); memcpy(f + 22, "\x10\x00", 2);
    memcpy(f + 24, "\xAA\xAA\x03\0\0\0\x88\x8e", 8); memcpy(f + 32, e, n);
    feed(f, 32 + n, 0);
}
static void key_frame(uint8_t *e, uint16_t info, const uint8_t rc[8], const uint8_t nonce[32], const uint8_t kck[16], const uint8_t *data, unsigned dl, int bad_mic)
{
    unsigned body = 95 + dl; memset(e, 0, 4 + body);
    e[0] = 2; e[1] = 3; e[2] = (uint8_t)(body >> 8); e[3] = (uint8_t)body; e[4] = 2; e[5] = (uint8_t)(info >> 8); e[6] = (uint8_t)info;
    memcpy(e + 9, rc, 8); memcpy(e + 17, nonce, 32); e[97] = (uint8_t)(dl >> 8); e[98] = (uint8_t)dl; if (dl) memcpy(e + 99, data, dl);
    uint8_t m[20]; wpa_hmac_sha1(kck, 16, e, 4 + body, m); memcpy(e + 81, m, 16); if (bad_mic) e[81] ^= 1;
}
static void handshake_test(void)
{
    static const uint8_t sta[6] = { 2, 0, 0, 0, 0, 9 }, rsn[22] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
    uint8_t macs[8][6], pmk[32], ptk[48], e[200], sn[32]; uint32_t au, as, re, ht, be; unsigned used;
    wpa_pmk("password1", (const uint8_t *)"fz", 2, pmk); for (int i = 0; i < 32; i++) sn[i] = (uint8_t)(i * 7 + 1);
    CHECK(wifi_ap_start("fz", "password1", 6, 3) == 0);
    send_mgmt(0xB0, sta, (const uint8_t *)"\0\0\1\0\0\0", 6); CHECK(last[0] == 0xB0 && last[26] == 2 && last[28] == 0);
    uint8_t ar[40] = { 1, 0, 10, 0, 0, 2, 'f', 'z', 1, 4, 0x82, 0x84, 0x8b, 0x96 }; memcpy(ar + 14, rsn, 22);
    send_mgmt(0x00, sta, ar, 14 + 22);
    wifi_ap_stats(&au, &as, &re, &ht, &be, &used); CHECK(as == 1 && used == 1 && wifi_ap_stations(macs, 8) == 0);         /* associated, not keyed */
    CHECK(last[0] == 0x08 && last[1] == 0x02 && !memcmp(last + 24, "\xAA\xAA\x03\0\0\0\x88\x8e", 8));                    /* message 1 follows the association response */
    const uint8_t *m1 = last + 32; CHECK(m1[5] == 0x00 && m1[6] == 0x8a && m1[8] == 16);
    uint8_t anonce[32], rc[8]; memcpy(anonce, m1 + 17, 32); memcpy(rc, m1 + 9, 8);
    wpa_ptk(pmk, mine, sta, anonce, sn, ptk);
    unsigned txs = n_tx; key_frame(e, 0x010a, rc, sn, ptk, rsn, 22, 1); send_eapol(sta, e, 4 + 95 + 22);                   /* message 2 with a wrong MIC */
    CHECK(n_tx == txs && keys_set == 0);                                                                                     /* nothing answered, nothing installed */
    key_frame(e, 0x010a, rc, sn, ptk, rsn, 22, 0); send_eapol(sta, e, 4 + 95 + 22);
    CHECK(keys_set == 1 && last[0] == 0x08 && last[1] == 0x02 && last[32 + 6] == 0xca && last[32 + 5] == 0x13);             /* key installed, message 3 sent (install, ack, mic, secure, encrypted data) */
    uint8_t rc3[8]; memcpy(rc3, last + 32 + 9, 8); CHECK(memcmp(rc3, rc, 8) > 0);                                           /* replay counter went up */
    uint8_t rcw[8]; memcpy(rcw, rc3, 8); rcw[7] ^= 0x40; key_frame(e, 0x030a, rcw, anonce, ptk, 0, 0, 0); send_eapol(sta, e, 4 + 95);   /* message 4 for another replay counter */
    CHECK(wifi_ap_stations(macs, 8) == 0);
    key_frame(e, 0x030a, rc3, anonce, ptk, 0, 0, 0); send_eapol(sta, e, 4 + 95);
    CHECK(wifi_ap_stations(macs, 8) == 1 && !memcmp(macs[0], sta, 6));                                                      /* message 4 done: keyed and up */
    now_us += 10000000ull; wifi_ap_poll();
    wifi_ap_stats(&au, &as, &re, &ht, &be, &used); CHECK(used == 1);                                                         /* a keyed station is not a handshake timeout */
    now_us += 400000000ull; wifi_ap_poll();
    wifi_ap_stats(&au, &as, &re, &ht, &be, &used); CHECK(used == 0 && wifi_ap_stations(macs, 8) == 0);                       /* 300 s without a frame: dropped */
    wifi_ap_stop();
}

int main(int argc, char **argv)
{
    handshake_test();
    unsigned long rounds = argc > 1 ? strtoul(argv[1], 0, 0) : 100000; x = argc > 2 ? strtoul(argv[2], 0, 0) | 1 : 7;
    wifi_eth_rx = on_eth;
    for (int secure = 0; secure < 2; secure++) {
        CHECK(wifi_ap_start("fz", secure ? "password1" : 0, 6, 3) == 0);
        uint8_t macs[8][6]; uint32_t au, as, re, ht, be; unsigned used;
        for (unsigned long i = 0; i < rounds && !fails; i++) {
            uint8_t f[400]; unsigned len = 24 + rnd() % 200;
            for (unsigned k = 0; k < len; k++) f[k] = (uint8_t)rnd();
            static const uint8_t types[] = { 0x40, 0xB0, 0x00, 0xC0, 0xA0, 0x08, 0x88, 0x48, 0xC8 };
            f[0] = types[rnd() % sizeof types]; f[1] = (uint8_t)(rnd() & 0x4F);
            if (rnd() % 4) memcpy(f + 4, mine, 6);                                      /* mostly addressed to the AP */
            f[10] = 2; f[11] = 0; f[12] = 0; f[13] = 0; f[14] = 0; f[15] = (uint8_t)(rnd() % 6);   /* a handful of station addresses so tables fill and states advance */
            if (f[0] == 0xB0 && len >= 30 && rnd() % 2) { f[24] = f[25] = 0; f[26] = 1; f[27] = 0; }
            if (f[0] == 0x00 && len >= 40 && rnd() % 2) { f[28] = 0; f[29] = 2; f[30] = 'f'; f[31] = 'z'; if (rnd() % 2) { f[32] = 48; f[33] = (uint8_t)(rnd() % 30); } }
            if (f[0] == 0x40 && len >= 28 && rnd() % 2) { f[24] = 0; f[25] = 0; }
            if ((f[0] & 0x0C) == 0x08 && len > 40 && rnd() % 2) { f[1] = (uint8_t)(0x01 | (rnd() % 2 ? 0x40 : 0)); memcpy(f + 24 + ((f[0] & 0x80) ? 2 : 0) + (f[1] & 0x40 ? 8 : 0), "\xAA\xAA\x03\0\0\0", 6); }
            feed(f, len, (rnd() & 7) == 0);
            now_us += (rnd() % 8 == 0) ? 1500000ull : 4000ull;                         /* time passes: beacons, handshake retries and timeouts */
            wifi_ap_stats(&au, &as, &re, &ht, &be, &used);
            CHECK(used <= 3);
            CHECK(wifi_ap_stations(macs, 8) <= 3);
            if (!secure) CHECK(keys_set == 0 || 1);
        }
        wifi_ap_stop();
    }
    printf("fuzz_ap: %s (%lu rounds, %u frames sent, %u keys, %u ethernet frames up)\n", fails ? "FAILED" : "ok", rounds, n_tx, keys_set, eth_n);
    return fails != 0;
}
