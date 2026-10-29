/* 802.11 frame builders and the data-frame to Ethernet conversion: pure. Frames are returned without FCS (wifi_mac_tx adds it). */
#include <string.h>
#include "wifi.h"

static const uint8_t bcast[6] = { 255, 255, 255, 255, 255, 255 };
static unsigned hdr(uint8_t *o, uint8_t fc0, uint8_t fc1, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3, uint16_t seq)
{
    o[0] = fc0; o[1] = fc1; o[2] = o[3] = 0;
    memcpy(o + 4, a1, 6); memcpy(o + 10, a2, 6); memcpy(o + 16, a3, 6);
    o[22] = (uint8_t)(seq << 4); o[23] = (uint8_t)(seq >> 4);
    return 24;
}
static unsigned ssid_ie(uint8_t *o, const char *ssid) { unsigned l = ssid ? (unsigned)strnlen(ssid, 32) : 0; o[0] = 0; o[1] = (uint8_t)l; if (l) memcpy(o + 2, ssid, l); return 2 + l; }
static unsigned rates_ie(uint8_t *o)                              /* 802.11b/g as an IDF station: 1, 2, 5.5, 11 (basic), 6..36 in Supported Rates, 48, 54 in Extended Supported Rates */
{
    static const uint8_t r[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6c };
    memcpy(o, r, sizeof r); return sizeof r;
}

unsigned wifi_build_probe_req(uint8_t *o, const uint8_t me[6], const char *ssid, uint16_t seq)
{
    unsigned n = hdr(o, 0x40, 0, bcast, me, bcast, seq);
    n += ssid_ie(o + n, ssid); n += rates_ie(o + n);
    return n;
}

unsigned wifi_build_auth(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], uint16_t seq)
{
    unsigned n = hdr(o, 0xB0, 0, bssid, me, bssid, seq);
    static const uint8_t body[6] = { 0, 0, 1, 0, 0, 0 };         /* open system, transaction 1, status 0 */
    memcpy(o + n, body, 6);
    return n + 6;
}

unsigned wifi_build_assoc_req(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], const char *ssid, int rsn, uint16_t seq)
{
    unsigned n = hdr(o, 0x00, 0, bssid, me, bssid, seq);
    o[n++] = 0x01; o[n++] = 0x00;                                 /* capability: ESS */
    o[n++] = 10; o[n++] = 0;                                      /* listen interval */
    n += ssid_ie(o + n, ssid); n += rates_ie(o + n);
    if (rsn) { static const uint8_t r[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 }; memcpy(o + n, r, sizeof r); n += sizeof r; }   /* RSN: CCMP group, CCMP pairwise, PSK */
    static const uint8_t ht[] = { 45, 26, 0x6e, 0x11, 0x00, 0xff, 0,0,0,0,0,0,0,0,0,0,0, 0x01, 0,0,0,0,0,0,0,0,0,0 };   /* HT capabilities as an IDF station sends them (captured): 0x116e, MCS 0-7, Tx MCS set defined */
    memcpy(o + n, ht, sizeof ht); n += sizeof ht;
    static const uint8_t wmm[] = { 0xdd, 7, 0x00, 0x50, 0xf2, 2, 0, 1, 0 };   /* WMM information element: a QoS station, so the AP sends QoS data with one TID per queue (as to an IDF station) */
    memcpy(o + n, wmm, sizeof wmm); n += sizeof wmm;
    return n;
}

unsigned wifi_build_deauth(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], uint16_t reason, uint16_t seq)
{
    unsigned n = hdr(o, 0xC0, 0, bssid, me, bssid, seq);
    o[n++] = (uint8_t)reason; o[n++] = (uint8_t)(reason >> 8);
    return n;
}

unsigned wifi_build_null(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], uint16_t seq) { return hdr(o, 0x48, 0x01, bssid, me, bssid, seq); }   /* keep-alive */

unsigned wifi_build_data(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n, uint16_t seq)
{
    unsigned k = hdr(o, 0x08, 0x01, bssid, me, dst, seq);         /* data, to DS */
    static const uint8_t snap[6] = { 0xAA, 0xAA, 0x03, 0, 0, 0 };
    memcpy(o + k, snap, 6); o[k + 6] = (uint8_t)(ethertype >> 8); o[k + 7] = (uint8_t)ethertype;
    memcpy(o + k + 8, p, n);
    return k + 8 + n;
}

int wifi_data_to_eth(const uint8_t *f, unsigned len, const uint8_t bssid[6], uint8_t *eth, unsigned max)
{
    if (len < 24 + 8 || (f[0] & 0x0C) != 0x08 || (f[1] & 0x03) != 0x02 || (f[1] & 0x40)) return -1;   /* data, from DS, not protected */
    if (memcmp(f + 10, bssid, 6)) return -1;                       /* transmitter must be our AP */
    unsigned h = 24 + ((f[0] & 0x80) ? 2 : 0);                     /* QoS adds 2 bytes */
    if (len < h + 8 || memcmp(f + h, "\xAA\xAA\x03\0\0\0", 6)) return -1;
    unsigned n = len - h - 8;
    if (14 + n > max) return -1;
    memcpy(eth, f + 4, 6); memcpy(eth + 6, f + 16, 6);            /* destination, source */
    eth[12] = f[h + 6]; eth[13] = f[h + 7];
    memcpy(eth + 14, f + h + 8, n);
    return (int)(14 + n);
}

/* --- access point side --- */
unsigned wifi_build_beacon(uint8_t *o, const uint8_t dst[6], const uint8_t bssid[6], const char *ssid, uint8_t chan, int rsn, uint64_t tsf, uint16_t seq, int probe_resp)
{
    unsigned n = hdr(o, probe_resp ? 0x50 : 0x80, 0, dst, bssid, bssid, seq);
    for (int i = 0; i < 8; i++) o[n++] = (uint8_t)(tsf >> 8 * i);
    o[n++] = 100; o[n++] = 0;                                     /* beacon interval: 100 TU */
    o[n++] = 0x01 | (rsn ? 0x10 : 0); o[n++] = 0;                 /* capability: ESS, privacy when protected */
    n += ssid_ie(o + n, ssid); n += rates_ie(o + n);
    o[n++] = 3; o[n++] = 1; o[n++] = chan;                        /* DS parameter set */
    static const uint8_t tim[] = { 5, 4, 0, 1, 0, 0 };            /* nothing buffered: no power-save stations */
    if (!probe_resp) { memcpy(o + n, tim, sizeof tim); n += sizeof tim; }
    if (rsn) { static const uint8_t r[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 }; memcpy(o + n, r, sizeof r); n += sizeof r; }
    return n;
}

unsigned wifi_build_auth_resp(uint8_t *o, const uint8_t dst[6], const uint8_t bssid[6], uint16_t status, uint16_t seq)
{
    unsigned n = hdr(o, 0xB0, 0, dst, bssid, bssid, seq);
    o[n++] = 0; o[n++] = 0; o[n++] = 2; o[n++] = 0; o[n++] = (uint8_t)status; o[n++] = (uint8_t)(status >> 8);   /* open system, transaction 2 */
    return n;
}

unsigned wifi_build_assoc_resp(uint8_t *o, const uint8_t dst[6], const uint8_t bssid[6], uint16_t status, uint16_t aid, int rsn, uint16_t seq)
{
    unsigned n = hdr(o, 0x10, 0, dst, bssid, bssid, seq);
    o[n++] = 0x01 | (rsn ? 0x10 : 0); o[n++] = 0; o[n++] = (uint8_t)status; o[n++] = (uint8_t)(status >> 8);
    aid |= 0xC000; o[n++] = (uint8_t)aid; o[n++] = (uint8_t)(aid >> 8);
    return n + rates_ie(o + n);
}

unsigned wifi_build_deauth_ap(uint8_t *o, const uint8_t dst[6], const uint8_t bssid[6], uint16_t reason, uint16_t seq)
{
    unsigned n = hdr(o, 0xC0, 0, dst, bssid, bssid, seq);
    o[n++] = (uint8_t)reason; o[n++] = (uint8_t)(reason >> 8);
    return n;
}

unsigned wifi_build_data_fromds(uint8_t *o, const uint8_t bssid[6], const uint8_t dst[6], const uint8_t src[6], uint16_t ethertype, const uint8_t *p, unsigned n, uint16_t seq)
{
    unsigned k = hdr(o, 0x08, 0x02, dst, bssid, src, seq);        /* data, from DS */
    static const uint8_t snap[6] = { 0xAA, 0xAA, 0x03, 0, 0, 0 };
    memcpy(o + k, snap, 6); o[k + 6] = (uint8_t)(ethertype >> 8); o[k + 7] = (uint8_t)ethertype;
    memcpy(o + k + 8, p, n);
    return k + 8 + n;
}

int wifi_ap_data_to_eth(const uint8_t *f, unsigned len, const uint8_t bssid[6], uint8_t *eth, unsigned max)
{
    if (len < 24 + 8 || (f[0] & 0x0C) != 0x08 || (f[1] & 0x03) != 0x01 || (f[1] & 0x40)) return -1;   /* data, to DS, not protected */
    if (memcmp(f + 4, bssid, 6)) return -1;                        /* receiver must be us */
    unsigned h = 24 + ((f[0] & 0x80) ? 2 : 0);
    if (len < h + 8 || memcmp(f + h, "\xAA\xAA\x03\0\0\0", 6)) return -1;
    unsigned n = len - h - 8;
    if (14 + n > max) return -1;
    memcpy(eth, f + 16, 6); memcpy(eth + 6, f + 10, 6);            /* destination (address 3), source (address 2) */
    eth[12] = f[h + 6]; eth[13] = f[h + 7];
    memcpy(eth + 14, f + h + 8, n);
    return (int)(14 + n);
}
