/* Wi-Fi station on the S3's own MAC (wifi_mac.h): scan, join an open network, Ethernet frames in and out. One consumer: wifi_poll() must be
 * called from the main loop while the link is in use. ie.c / frame.c are pure (no radio): the host tests and the fuzzer drive them. */
#pragma once
#include <stdint.h>

typedef struct { uint8_t bssid[6], chan; int8_t rssi; char ssid[33]; uint8_t sec; /* 0 open, 1 other privacy (WEP, WPA), 2 WPA2 / WPA3 (RSN element) */ uint16_t beacon_tu;
                 uint8_t sec40;   /* HT Operation: 40 MHz with the secondary channel 1 = above, 3 = below; 0 = 20 MHz only */
                 uint16_t akm, rsn_caps; /* RSN: bit n = AKM suite 00-0f-ac:n (2 PSK, 6 PSK-SHA256, 8 SAE); capabilities (0x80 MFPC, 0x40 MFPR) */ } wifi_ap_t;

/* --- pure helpers (ie.c, frame.c) --- */
int      wifi_ie_find(const uint8_t *ies, unsigned n, uint8_t id, const uint8_t **val);   /* length of the first IE with that id, -1 = none or malformed */
int      wifi_parse_beacon(const uint8_t *f, unsigned len, wifi_ap_t *ap);                /* beacon or probe response; 0 ok, -1 malformed */
unsigned wifi_build_probe_req(uint8_t *o, const uint8_t me[6], const char *ssid, uint16_t seq);   /* ssid NULL = wildcard; returns the frame length (no FCS) */
unsigned wifi_build_auth(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], uint16_t seq);
unsigned wifi_build_assoc_req(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], const char *ssid, const uint8_t *rsn, uint16_t seq);   /* rsn: the RSN element to send (id, length, body), NULL = open */
unsigned wifi_build_deauth(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], uint16_t reason, uint16_t seq);
unsigned wifi_build_null(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], uint16_t seq);   /* null data frame, to DS */
unsigned wifi_build_qos_data(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n, uint16_t seq, uint8_t tid);   /* QoS data, to DS, normal ack policy */
unsigned wifi_build_data(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n, uint16_t seq);
int      wifi_amsdu_to_eth(const uint8_t *f, unsigned len, const uint8_t bssid[6], unsigned *off, uint8_t *eth, unsigned max);   /* next A-MSDU subframe as Ethernet; *off = 0 first; 0 = end, -1 = malformed */
int      wifi_data_to_eth(const uint8_t *f, unsigned len, const uint8_t bssid[6], uint8_t *eth, unsigned max);   /* from-DS data frame -> 14-byte header + payload; length, -1 = not for us / malformed */

/* access point side: all pure, frames without FCS */
unsigned wifi_build_beacon(uint8_t *o, const uint8_t dst[6], const uint8_t bssid[6], const char *ssid, uint8_t chan, int rsn, uint64_t tsf, uint16_t seq, int probe_resp);   /* beacon (dst broadcast) or probe response (dst = the asker) */
unsigned wifi_build_auth_resp(uint8_t *o, const uint8_t dst[6], const uint8_t bssid[6], uint16_t status, uint16_t seq);
unsigned wifi_build_assoc_resp(uint8_t *o, const uint8_t dst[6], const uint8_t bssid[6], uint16_t status, uint16_t aid, int rsn, uint16_t seq);
unsigned wifi_build_deauth_ap(uint8_t *o, const uint8_t dst[6], const uint8_t bssid[6], uint16_t reason, uint16_t seq);
unsigned wifi_build_data_fromds(uint8_t *o, const uint8_t bssid[6], const uint8_t dst[6], const uint8_t src[6], uint16_t ethertype, const uint8_t *p, unsigned n, uint16_t seq);
int      wifi_ap_data_to_eth(const uint8_t *f, unsigned len, const uint8_t bssid[6], uint8_t *eth, unsigned max);   /* to-DS data frame for us -> Ethernet; -1 = not ours / malformed */

/* --- access point (ap.c): one network at a time, shares the radio with the station (use one or the other) --- */
int      wifi_ap_start(const char *ssid, const char *pass /* NULL = open, else WPA2-PSK */, uint8_t chan, unsigned max_sta /* <= NDF_WIFI_MAX_STA */);   /* after wifi_init(); 0 ok */
void     wifi_ap_stop(void);
unsigned wifi_ap_stations(uint8_t macs[][6], unsigned max);               /* associated (and, when protected, keyed) stations */
void     wifi_ap_stats(uint32_t *auth, uint32_t *assoc, uint32_t *refused, uint32_t *hs_timeout, uint32_t *beacons, unsigned *used);   /* counters since boot; used = station slots in use (any state) */
void     wifi_ap_poll(void);                                              /* rx dispatch, beacons, timeouts: call from the main loop */
int      wifi_ap_send_eth(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n);   /* from the AP's own address; 0 sent */
const uint8_t *wifi_mac_addr(void);                                       /* the address given to wifi_init() */

/* --- station (sta.c) --- */
int  wifi_init(const uint8_t mac[6]);                                      /* radio on, listening */
int  wifi_scan(wifi_ap_t *out, unsigned max, unsigned *n);                 /* active, channels 1..13, 120 ms dwell */
int  wifi_sta_join(const char *ssid, const char *pass /* NULL = open */, uint32_t timeout_ms);   /* 0 joined, -1 not found / timeout / refused, -2 network security not supported, -3 4-way handshake failed (wrong passphrase) */
void wifi_set_data_rate(unsigned rate);                                    /* PHY rate code for data frames (WIFI_RATE_*; default 12 Mbit/s) */
int  wifi_sta_up(void);                                                    /* 1 while associated */
void wifi_sta_stats(uint32_t *gtk_updates, uint32_t *grp_rx);             /* group keys installed (rekeys included), protected group frames accepted */
void wifi_poll(void);                                                      /* rx dispatch, beacon timeout, deauth */
int  wifi_send_eth(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n);   /* 0 sent */
extern void (*wifi_eth_rx)(const uint8_t *frame, unsigned n);              /* 14-byte Ethernet header + payload */
