/* Wi-Fi commands of the peer: access point, station, sniffer (frame counts), raw frame injection. */
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

static void on_event(void *a, esp_event_base_t base, int32_t id, void *d)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = d; printf("event ap-join %02x:%02x:%02x:%02x:%02x:%02x\n", e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = d; printf("event ap-leave %02x:%02x:%02x:%02x:%02x:%02x\n", e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) printf("event sta-connected\n");
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
    if (strcmp(argv[2], "-")) { strlcpy((char *)c.ap.password, argv[2], sizeof c.ap.password); c.ap.authmode = WIFI_AUTH_WPA2_PSK; }
    else c.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_AP); esp_wifi_set_config(WIFI_IF_AP, &c);
    int rc = esp_wifi_start(); mode_on = !rc;
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
    esp_wifi_connect();
    return (xEventGroupWaitBits(evg, GOT_IP, 0, 0, pdMS_TO_TICKS(20000)) & GOT_IP) ? 0 : -2;
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
static int cmd_chan(int argc, char **argv) { return argc < 2 ? -1 : esp_wifi_set_channel(atoi(argv[1]), WIFI_SECOND_CHAN_NONE); }

const peer_cmd_t cmds_wifi[] = { { "wifi_ap", cmd_ap }, { "wifi_sta", cmd_sta }, { "wifi_stop", cmd_stop }, { "wifi_sniff", cmd_sniff },
                                 { "wifi_raw", cmd_raw }, { "wifi_chan", cmd_chan }, { "wifi_unicast", cmd_unicast }, { 0 } };
