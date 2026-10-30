#include "esp_timer.h"
/* Wi-Fi commands of the peer: access point, station, sniffer (frame counts), raw frame injection. */
#include "esp_private/wifi.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "../peer.h"

#define GOT_IP BIT0
static int inited, mode_on;
static EventGroupHandle_t evg;
static esp_netif_t *sta_if, *ap_if;
static volatile uint32_t sn_mgmt, sn_ctrl, sn_data, sn_beacon, sn_rate_sum, sn_n;

static int64_t t_conn;                                           /* connected event (cmd_sta prints join / total time from the connect call) */
static void on_event(void *a, esp_event_base_t base, int32_t id, void *d)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = d; printf("event ap-join %02x:%02x:%02x:%02x:%02x:%02x\n", e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = d; printf("event ap-leave %02x:%02x:%02x:%02x:%02x:%02x\n", e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_PROBEREQRECVED) {
        wifi_event_ap_probe_req_rx_t *e = d; printf("event ap-probe from %02x:%02x:%02x:%02x:%02x:%02x rssi %d\n", e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->rssi);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) { t_conn = esp_timer_get_time(); printf("event sta-connected\n"); }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = d; printf("event sta-disconnected reason %d\n", e->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = d; printf("sta ip " IPSTR "\n", IP2STR(&e->ip_info.ip)); xEventGroupSetBits(evg, GOT_IP);
    }
}

static int wifi_boot(void)
{
    if (inited) return 0;
    evg = xEventGroupCreate();
    sta_if = esp_netif_create_default_wifi_sta(); ap_if = esp_netif_create_default_wifi_ap();
    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&c)) return -1;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, 0);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, 0);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_country_code("CN", true);                       /* channels 1..13 (the default world-safe set stops at 11) */
    esp_wifi_set_event_mask(0);                                  /* deliver the AP probe-request event too */
    inited = 1;
    return 0;
}
static void halt(void) { if (mode_on) { esp_wifi_stop(); mode_on = 0; } }

static int cmd_ap(int argc, char **argv)                         /* wifi_ap <ssid> <pass|-> <chan> */
{
    if (wifi_boot() || argc < 4) return -1;
    halt();
    wifi_config_t c = { 0 };
    strlcpy((char *)c.ap.ssid, argv[1], sizeof c.ap.ssid); c.ap.ssid_len = strlen(argv[1]);
    c.ap.channel = atoi(argv[3]); c.ap.max_connection = 4; c.ap.beacon_interval = 100;
    if (strcmp(argv[2], "-")) {                                  /* pass, or <mode>:pass with mode pmf (WPA2, PMF capable), pmfreq (PMF required), wpa3 (SAE), wpa23 (WPA2/WPA3 transition) */
        const char *pw = argv[2], *col = strchr(pw, ':'); c.ap.authmode = WIFI_AUTH_WPA2_PSK;
        if (col) {
            unsigned ml = (unsigned)(col - pw);
            if (ml == 3 && !strncmp(pw, "pmf", 3)) c.ap.pmf_cfg.capable = true;
            else if (ml == 6 && !strncmp(pw, "pmfreq", 6)) { c.ap.pmf_cfg.capable = true; c.ap.pmf_cfg.required = true; }
            else if (ml == 4 && !strncmp(pw, "wpa3", 4)) { c.ap.authmode = WIFI_AUTH_WPA3_PSK; c.ap.pmf_cfg.required = true; }
            else if (ml == 5 && !strncmp(pw, "wpa23", 5)) { c.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK; c.ap.pmf_cfg.capable = true; }
            else return -1;
            pw = col + 1;
        }
        strlcpy((char *)c.ap.password, pw, sizeof c.ap.password);
    } else c.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_AP); esp_wifi_set_config(WIFI_IF_AP, &c);
    if (argc > 4 && strcmp(argv[4], "-") && strtol(argv[4], 0, 0) < WIFI_PHY_RATE_MCS0_LGI) esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);   /* a fixed legacy rate needs the 802.11n aggregation off; an MCS code keeps 11n */
    int bw20 = argc > 5 && !strcmp(argv[5], "20");            /* wifi_ap <ssid> <pass> <chan> <rate|-> 20: HT20 instead of IDF's default HT40 */
    int rc = esp_wifi_start(); mode_on = !rc;
    if (!rc && bw20) esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    if (!rc && argc > 4 && strcmp(argv[4], "-")) esp_wifi_internal_set_fix_rate(WIFI_IF_AP, true, (wifi_phy_rate_t)strtol(argv[4], 0, 0));   /* optional: fixed PHY rate code (IDF wifi_phy_rate_t), same on every device compared */
    if (!rc) { esp_netif_dhcps_stop(ap_if); esp_netif_dhcps_start(ap_if); }      /* the DHCP server stops answering after a few AP restarts otherwise (seen on this peer) */
    uint8_t m[6]; esp_wifi_get_mac(WIFI_IF_AP, m);
    printf("ap %s ch %d bssid %02x:%02x:%02x:%02x:%02x:%02x ip 192.168.4.1\n", argv[1], c.ap.channel, m[0], m[1], m[2], m[3], m[4], m[5]);
    return rc;
}
static int cmd_sta(int argc, char **argv)                        /* wifi_sta <ssid> <pass|-> */
{
    if (wifi_boot() || argc < 3) return -1;
    halt();
    wifi_config_t c = { 0 };
    strlcpy((char *)c.sta.ssid, argv[1], sizeof c.sta.ssid);
    if (strcmp(argv[2], "-")) { strlcpy((char *)c.sta.password, argv[2], sizeof c.sta.password); c.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; }
    xEventGroupClearBits(evg, GOT_IP);
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &c);
    int rc = esp_wifi_start(); mode_on = !rc;
    if (rc) return rc;
    esp_wifi_set_ps(WIFI_PS_NONE);                               /* the esp-ndf access point has no power-save buffering (v1) */
    if (argc > 3 && !strcmp(argv[3], "bg")) esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);   /* wifi_sta <ssid> <pass> bg: no 802.11n (to isolate IDF's HT settings) */
    int64_t t0 = esp_timer_get_time(); t_conn = 0;
    esp_wifi_connect();
    int ok = (xEventGroupWaitBits(evg, GOT_IP, 0, 0, pdMS_TO_TICKS(20000)) & GOT_IP) != 0;
    if (ok) printf("sta join_ms %d total_ms %d\n", t_conn ? (int)((t_conn - t0) / 1000) : -1, (int)((esp_timer_get_time() - t0) / 1000));
    return ok ? 0 : -2;
}
static int cmd_stop(int argc, char **argv) { halt(); return 0; }

static void sniff_cb(void *buf, wifi_promiscuous_pkt_type_t t)
{
    wifi_promiscuous_pkt_t *p = buf;
    if (t == WIFI_PKT_MGMT) { sn_mgmt++; if ((p->payload[0] & 0xFC) == 0x80) sn_beacon++; }
    else if (t == WIFI_PKT_CTRL) sn_ctrl++; else sn_data++;
    sn_rate_sum += p->rx_ctrl.rate; sn_n++;
}
static int cmd_sniff(int argc, char **argv)                      /* wifi_sniff <chan> <seconds> */
{
    if (wifi_boot() || argc < 3) return -1;
    halt();
    sn_mgmt = sn_ctrl = sn_data = sn_beacon = sn_rate_sum = sn_n = 0;
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_start(); mode_on = 1;
    esp_wifi_set_channel(atoi(argv[1]), WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous_rx_cb(sniff_cb); esp_wifi_set_promiscuous(true);
    vTaskDelay(pdMS_TO_TICKS(atoi(argv[2]) * 1000));
    esp_wifi_set_promiscuous(false);
    printf("sniff mgmt=%u ctrl=%u data=%u beacons=%u\n", (unsigned)sn_mgmt, (unsigned)sn_ctrl, (unsigned)sn_data, (unsigned)sn_beacon);
    return 0;
}
/* IDF refuses to transmit deauthentication and similar frames raw unless this hook says yes (used to forge one from a foreign BSSID) */
int ieee80211_raw_frame_sanity_check(int32_t a, int32_t b, int32_t c) { return 0; }

static int cmd_raw(int argc, char **argv)                        /* wifi_raw <ap|sta> <hex frame> */
{
    uint8_t f[512];
    if (argc < 3) return -1;
    int n = hex2bin(argv[2], f, sizeof f);
    return esp_wifi_80211_tx(!strcmp(argv[1], "ap") ? WIFI_IF_AP : WIFI_IF_STA, f, n, true);
}

/* wifi_unicast <chan> <dst mac> <n> [rate 1m|6m|54m] [len]: send n directed data frames (peer STA mac as source) 30 ms apart and
 * count the ACK control frames addressed to us that the sniffer hears: tells whether the destination acknowledged. */
static volatile uint32_t uc_acks; static uint8_t uc_ta[6];
static void uc_cb(void *buf, wifi_promiscuous_pkt_type_t t)
{
    wifi_promiscuous_pkt_t *p = buf;
    if (t == WIFI_PKT_CTRL && p->payload[0] == 0xD4 && !memcmp(p->payload + 4, uc_ta, 6)) uc_acks++;
}
static int cmd_unicast(int argc, char **argv)
{
    if (wifi_boot() || argc < 4) return -1;
    halt();
    unsigned v[6]; if (sscanf(argv[2], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -2;
    int n = atoi(argv[3]); unsigned len = argc > 5 ? atoi(argv[5]) : 60; if (len < 32) len = 32; if (len > 400) len = 400;
    wifi_phy_rate_t rate = WIFI_PHY_RATE_6M;
    if (argc > 4 && !strcmp(argv[4], "1m")) rate = WIFI_PHY_RATE_1M_L; else if (argc > 4 && !strcmp(argv[4], "54m")) rate = WIFI_PHY_RATE_54M;
    else if (argc > 4 && !strncmp(argv[4], "0x", 2)) rate = (wifi_phy_rate_t)strtol(argv[4], 0, 0);   /* any IDF rate code, e.g. 0x10 MCS0, 0x17 MCS7 */
    int ap = argc > 6 && !strcmp(argv[6], "ap");               /* send from the AP interface (an unassociated STA drops raw frames) */
    wifi_interface_t ifx = ap ? WIFI_IF_AP : WIFI_IF_STA;
    if (ap) {
        wifi_config_t c = { 0 }; strcpy((char *)c.ap.ssid, "SPIKE"); c.ap.ssid_len = 5; c.ap.channel = atoi(argv[1]); c.ap.max_connection = 2; c.ap.beacon_interval = 1000;
        esp_wifi_set_mode(WIFI_MODE_AP); esp_wifi_set_config(WIFI_IF_AP, &c); esp_wifi_start();
    } else { esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_start(); esp_wifi_set_channel(atoi(argv[1]), WIFI_SECOND_CHAN_NONE); }
    mode_on = 1;
    esp_wifi_config_80211_tx_rate(ifx, rate);
    esp_wifi_get_mac(ifx, uc_ta);
    wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL }; esp_wifi_set_promiscuous_filter(&f);
    uc_acks = 0; esp_wifi_set_promiscuous_rx_cb(uc_cb); esp_wifi_set_promiscuous(true);
    /* directed probe request (management frames are always sent; raw data frames to a non-associated station are dropped) */
    uint8_t fr[400] = { 0x40, 0x00, 0, 0 };
    for (int i = 0; i < 6; i++) { fr[4 + i] = v[i]; fr[10 + i] = uc_ta[i]; fr[16 + i] = v[i]; }
    static const uint8_t ies[] = { 0x00, 0x00, 0x01, 0x04, 0x82, 0x84, 0x8b, 0x96 };
    memcpy(fr + 24, ies, sizeof ies);
    for (unsigned i = 24 + sizeof ies; i < len; i++) fr[i] = (uint8_t)i;     /* padding (vendor-less filler) to the requested length */
    int txerr = 0, lastrc = 0;
    for (int i = 0; i < n; i++) { lastrc = esp_wifi_80211_tx(ifx, fr, len, true); txerr += lastrc != 0; vTaskDelay(pdMS_TO_TICKS(30)); }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_set_promiscuous(false);
    printf("unicast sent=%d txerr=%d lastrc=0x%x acks=%u\n", n, txerr, lastrc, (unsigned)uc_acks);
    return 0;
}

/* wifi_watch <mac>: promiscuous counter of frames whose transmitter address is <mac> (works next to a running AP); wifi_watch_stats prints it */
static uint8_t watch_mac[6]; static volatile uint32_t w_n, w_acks; static uint8_t w_all;
static struct { uint8_t fc0, fc1; uint16_t len; int8_t rssi; uint8_t rate, sig, mcs; uint8_t ra[6], ta[6]; uint32_t ts; uint8_t pl[64]; } w_log[64];
static void watch_cb(void *buf, wifi_promiscuous_pkt_type_t t)
{
    wifi_promiscuous_pkt_t *p = buf; unsigned len = p->rx_ctrl.sig_len;
    if (t == WIFI_PKT_CTRL && p->payload[0] == 0xD4 && !memcmp(p->payload + 4, watch_mac, 6)) w_acks++;           /* an ACK addressed to the watched station */
    int from = len >= 16 && !memcmp(p->payload + 10, watch_mac, 6), to = !memcmp(p->payload + 4, watch_mac, 6);
    if (p->payload[0] == 0x80 || !(from || (w_all && to))) return;                                                /* beacons are skipped; "all" adds frames addressed to the station (ACKs, downlink) */
    if (w_n < 64) { w_log[w_n] = (typeof(w_log[0])){ p->payload[0], p->payload[1], len, p->rx_ctrl.rssi, p->rx_ctrl.rate, p->rx_ctrl.sig_mode, p->rx_ctrl.mcs }; memcpy(w_log[w_n].ra, p->payload + 4, 6); if (len >= 16) memcpy(w_log[w_n].ta, p->payload + 10, 6); w_log[w_n].ts = p->rx_ctrl.timestamp; memcpy(w_log[w_n].pl, p->payload, len < 64 ? len : 64); }
    w_n++;
}
/* wifi_bcn <bssid> <n> <ms>: timestamps (us, from the radio) of up to n beacons of that BSSID within ms; prints the intervals' statistics. Needs wifi_mon first. */
static uint8_t bcn_mac[6]; static uint32_t bcn_ts[64]; static volatile uint32_t bcn_n, bcn_max;
static void bcn_cb(void *buf, wifi_promiscuous_pkt_type_t t)
{
    wifi_promiscuous_pkt_t *p = buf;
    if (t == WIFI_PKT_MGMT && p->payload[0] == 0x80 && !memcmp(p->payload + 10, bcn_mac, 6) && bcn_n < bcn_max) bcn_ts[bcn_n++] = p->rx_ctrl.timestamp;
}
static int cmd_bcn(int argc, char **argv)
{
    if (wifi_boot() || argc < 4) return -1;
    unsigned v[6]; if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -2;
    for (int i = 0; i < 6; i++) bcn_mac[i] = v[i];
    bcn_max = atoi(argv[2]) > 64 ? 64 : atoi(argv[2]); bcn_n = 0;
    wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT }; esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(bcn_cb); esp_wifi_set_promiscuous(true);
    vTaskDelay(pdMS_TO_TICKS(atoi(argv[3])));
    esp_wifi_set_promiscuous(false);
    uint32_t sum = 0, mn = ~0u, mx = 0, n = 0; double sq = 0;
    for (uint32_t i = 1; i < bcn_n; i++) { uint32_t d = bcn_ts[i] - bcn_ts[i - 1]; if (d > 150000) continue; sum += d; n++; if (d < mn) mn = d; if (d > mx) mx = d; }
    double mean = n ? (double)sum / n : 0; for (uint32_t i = 1; i < bcn_n; i++) { uint32_t d = bcn_ts[i] - bcn_ts[i - 1]; if (d > 150000) continue; sq += (d - mean) * (d - mean); }
    printf("bcn heard %u intervals %u mean_us %u min_us %u max_us %u dev_us %u\n", (unsigned)bcn_n, (unsigned)n, (unsigned)mean, (unsigned)(n ? mn : 0), (unsigned)mx, (unsigned)(n ? (uint32_t)__builtin_sqrt(sq / n) : 0));
    return 0;
}
static int cmd_mon(int argc, char **argv)                           /* wifi_mon <chan>: radio on that channel, not associated, for wifi_watch */
{
    if (wifi_boot() || argc < 2) return -1;
    halt();
    esp_wifi_set_mode(WIFI_MODE_STA); int rc = esp_wifi_start(); mode_on = !rc;
    return rc ? rc : esp_wifi_set_channel(atoi(argv[1]), WIFI_SECOND_CHAN_NONE);
}
static int cmd_watch(int argc, char **argv)
{
    if (wifi_boot() || argc < 2) return -1;
    unsigned v[6]; if (sscanf(argv[1], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -2;
    for (int i = 0; i < 6; i++) watch_mac[i] = v[i];
    w_n = w_acks = 0; w_all = argc > 2;                                  /* wifi_watch <mac> all: also frames addressed to <mac> */
    wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL }; esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(watch_cb); return esp_wifi_set_promiscuous(true);
}
static int cmd_watch_stats(int argc, char **argv)
{
    printf("watch frames=%u acks-to-it=%u\n", (unsigned)w_n, (unsigned)w_acks);
    for (unsigned i = 0; i < w_n && i < 64; i++) printf("  t %u fc %02x%02x len %u rate %u sig %u mcs %u ra %02x:%02x:%02x:%02x:%02x:%02x ta %02x:%02x:%02x:%02x:%02x:%02x\n", (unsigned)w_log[i].ts, w_log[i].fc0, w_log[i].fc1, w_log[i].len, w_log[i].rate, w_log[i].sig, w_log[i].mcs, w_log[i].ra[0], w_log[i].ra[1], w_log[i].ra[2], w_log[i].ra[3], w_log[i].ra[4], w_log[i].ra[5], w_log[i].ta[0], w_log[i].ta[1], w_log[i].ta[2], w_log[i].ta[3], w_log[i].ta[4], w_log[i].ta[5]);
    for (unsigned i = 0; i < w_n && i < 64; i++) { if (w_log[i].len < 100) continue; printf("  pl %u:", (unsigned)w_log[i].ts); for (int k = 0; k < 64; k++) printf("%02x", w_log[i].pl[k]); printf("\n"); }
    return 0;
}
static int cmd_stas(int argc, char **argv)                          /* wifi_stas: stations of the AP and the addresses its DHCP server gave out */
{
    wifi_sta_list_t l; if (esp_wifi_ap_get_sta_list(&l)) return -1;
    printf("stas %d\n", l.num);
    for (int i = 0; i < l.num; i++) printf("  %02x:%02x:%02x:%02x:%02x:%02x rssi %d\n", l.sta[i].mac[0], l.sta[i].mac[1], l.sta[i].mac[2], l.sta[i].mac[3], l.sta[i].mac[4], l.sta[i].mac[5], l.sta[i].rssi);
    return 0;
}
static int cmd_dhcps(int argc, char **argv)                         /* wifi_dhcps [restart]: status of the AP's DHCP server */
{
    esp_netif_dhcp_status_t st; esp_netif_dhcps_get_status(ap_if, &st);
    esp_netif_ip_info_t ip; esp_netif_get_ip_info(ap_if, &ip);
    printf("dhcps status %d ip " IPSTR "\n", (int)st, IP2STR(&ip.ip));
    if (argc > 1) { esp_netif_dhcps_stop(ap_if); int rc = esp_netif_dhcps_start(ap_if); esp_netif_dhcps_get_status(ap_if, &st); printf("restarted rc %d status %d\n", rc, (int)st); }
    return 0;
}
static int cmd_rd(int argc, char **argv)                            /* rd <addr> <n>: n words of a register block */
{
    if (argc < 3) return -1;
    uint32_t a = strtoul(argv[1], 0, 0); int n = atoi(argv[2]);
    for (int i = 0; i < n; i++) printf("%08x %08x\n", (unsigned)(a + 4 * i), (unsigned)*(volatile uint32_t *)(a + 4 * i));
    return 0;
}
/* i2crd <block>: analog registers 0..31 of a regi2c block, read with the same raw protocol as components/drivers/rf/rf_regi2c.c (for the IDF vs ndf diff) */
static int cmd_i2crd(int argc, char **argv)
{
    static const uint32_t mask[13] = { 0x400000, 0x800, 0x200, 0x100, 0x10000, 0x20000, 0x400, 0x8000, 0x40000, 0x2000, 0x1000, 0, 0x4000 };
    if (argc < 2) return -1;
    uint8_t b = (uint8_t)strtoul(argv[1], 0, 0); unsigned i = (uint8_t)(b - 0x62), m = (i <= 9 && ((0x227u >> i) & 1)) ? 1 : 0, j = (uint8_t)(b - 0x61);
    volatile uint32_t *mst = (volatile uint32_t *)(0x6000E000u + 4 * m);
    printf("i2c %02x", b);
    for (unsigned r = 0; r < 32; r++) {
        *(volatile uint32_t *)0x6000E048 = (*(volatile uint32_t *)0x6000E048 & 0xFFFE000Fu) | 0x0001FE00u;
        for (int n = 0; (*mst & (1u << 25)) && n < 100000; n++) {}
        *(volatile uint32_t *)0x6000E044 = ~(j < 13 ? mask[j] : 0u);
        *mst = 0x04000000u | r << 8 | b;
        for (int n = 0; (*mst & (1u << 25)) && n < 100000; n++) {}
        printf(" %02x", (unsigned)((*mst >> 16) & 0xFF));
    }
    printf("\n"); return 0;
}
static int cmd_kick(int argc, char **argv) { return esp_wifi_deauth_sta(0); }   /* wifi_kick: the AP deauthenticates every station (protected when PMF is on) */
static int cmd_pbusrd(int argc, char **argv) { unsigned (*rd)(unsigned, unsigned) = (unsigned (*)(unsigned, unsigned))0x40035b28; for (unsigned r = 0; r < 8; r++) { printf("pbus %u", r); for (unsigned c = 0; c < 8; c++) printf(" %03x", rd(r, c)); printf("\n"); } return 0; }   /* ROM rom_pbus_rd, as in ip_unit */
static int cmd_chan(int argc, char **argv) { return argc < 2 ? -1 : esp_wifi_set_channel(atoi(argv[1]), WIFI_SECOND_CHAN_NONE); }

const peer_cmd_t cmds_wifi[] = { { "wifi_ap", cmd_ap }, { "wifi_sta", cmd_sta }, { "wifi_stop", cmd_stop }, { "wifi_sniff", cmd_sniff },
                                 { "wifi_raw", cmd_raw }, { "wifi_chan", cmd_chan }, { "wifi_watch", cmd_watch }, { "wifi_watch_stats", cmd_watch_stats }, { "wifi_stas", cmd_stas }, { "wifi_mon", cmd_mon }, { "wifi_bcn", cmd_bcn }, { "wifi_dhcps", cmd_dhcps }, { "rd", cmd_rd }, { "pbusrd", cmd_pbusrd }, { "wifi_kick", cmd_kick }, { "i2crd", cmd_i2crd }, { "wifi_unicast", cmd_unicast }, { 0 } };
