/* Wi-Fi station on the S3's own MAC (wifi_mac.h): scan, join an open network, Ethernet frames in and out. One consumer: wifi_poll() must be
 * called from the main loop while the link is in use. ie.c / frame.c are pure (no radio): the host tests and the fuzzer drive them. */
#pragma once
#include <stdint.h>

typedef struct { uint8_t bssid[6], chan; int8_t rssi; char ssid[33]; uint8_t sec; /* 0 open, 1 WPA/WPA2 */ uint16_t beacon_tu; } wifi_ap_t;

/* --- pure helpers (ie.c, frame.c) --- */
int      wifi_ie_find(const uint8_t *ies, unsigned n, uint8_t id, const uint8_t **val);   /* length of the first IE with that id, -1 = none or malformed */
int      wifi_parse_beacon(const uint8_t *f, unsigned len, wifi_ap_t *ap);                /* beacon or probe response; 0 ok, -1 malformed */
unsigned wifi_build_probe_req(uint8_t *o, const uint8_t me[6], const char *ssid, uint16_t seq);   /* ssid NULL = wildcard; returns the frame length (no FCS) */
unsigned wifi_build_auth(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], uint16_t seq);
unsigned wifi_build_assoc_req(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], const char *ssid, uint16_t seq);
unsigned wifi_build_deauth(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], uint16_t reason, uint16_t seq);
unsigned wifi_build_null(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], uint16_t seq);   /* null data frame, to DS */
unsigned wifi_build_data(uint8_t *o, const uint8_t me[6], const uint8_t bssid[6], const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n, uint16_t seq);
int      wifi_data_to_eth(const uint8_t *f, unsigned len, const uint8_t bssid[6], uint8_t *eth, unsigned max);   /* from-DS data frame -> 14-byte header + payload; length, -1 = not for us / malformed */

/* --- station (sta.c) --- */
int  wifi_init(const uint8_t mac[6]);                                      /* radio on, listening */
int  wifi_scan(wifi_ap_t *out, unsigned max, unsigned *n);                 /* active, channels 1..13, 120 ms dwell */
int  wifi_sta_join(const char *ssid, const char *pass /* NULL = open */, uint32_t timeout_ms);   /* 0 joined, -1 timeout / refused, -2 WPA not available */
void wifi_set_data_rate(unsigned rate);                                    /* PHY rate code for data frames (WIFI_RATE_*; default 12 Mbit/s) */
int  wifi_sta_up(void);                                                    /* 1 while associated */
void wifi_poll(void);                                                      /* rx dispatch, beacon timeout, deauth */
int  wifi_send_eth(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n);   /* 0 sent */
extern void (*wifi_eth_rx)(const uint8_t *frame, unsigned n);              /* 14-byte Ethernet header + payload */
