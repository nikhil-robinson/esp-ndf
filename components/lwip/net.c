/* lwIP netif on the Wi-Fi station: Ethernet frames in from wifi_eth_rx, out through wifi_send_eth. */
#include <string.h>
#include "net.h"
#include "wifi.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "netif/ethernet.h"

static struct netif nif;
/* the access point's functions come in as callbacks: an image that never starts it does not reference ap.c (its buffers would count against the RAM limit) */
static int  (*ap_send)(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, unsigned n);
static void (*ap_poll)(void);                                  /* netif on the access point instead of the station */
static uint8_t txb[1600];

static err_t wifi_output(struct netif *n, struct pbuf *p)
{
    (void)n;
    if (p->tot_len < 14 || p->tot_len > sizeof txb) return ERR_BUF;
    pbuf_copy_partial(p, txb, p->tot_len, 0);                   /* one flat frame: 14-byte Ethernet header + payload */
    LINK_STATS_INC(link.xmit);
    return (ap_send ? ap_send : wifi_send_eth)(txb, (uint16_t)(txb[12] << 8 | txb[13]), txb + 14, p->tot_len - 14) ? ERR_IF : ERR_OK;   /* destination = first 6 bytes of txb */
}

static void eth_in(const uint8_t *f, unsigned n)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
    if (!p) { LINK_STATS_INC(link.memerr); LINK_STATS_INC(link.drop); return; }       /* pool empty: counted in lwip_stats.link */
    pbuf_take(p, f, (u16_t)n);
    LINK_STATS_INC(link.recv);
    if (nif.input(p, &nif) != ERR_OK) pbuf_free(p);
}

static err_t if_init(struct netif *n)
{
    n->name[0] = 'w'; n->name[1] = 'f';
    n->output = etharp_output;
    n->linkoutput = wifi_output;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    n->hostname = "esp-ndf";
    return ERR_OK;
}

int net_start(const uint8_t mac[6])
{
    static int started;
    if (started) { dhcp_stop(&nif); return dhcp_start(&nif) == ERR_OK ? 0 : -1; }   /* again after a new join: same interface, fresh lease (a second lwip_init / netif_add would loop the netif list) */
    started = 1;
    lwip_init();
    ip4_addr_t z = { 0 };
    if (!netif_add(&nif, &z, &z, &z, 0, if_init, ethernet_input)) return -1;
    memcpy(nif.hwaddr, mac, 6);
    netif_set_default(&nif);
    netif_set_up(&nif);
    wifi_eth_rx = eth_in;
    return dhcp_start(&nif) == ERR_OK ? 0 : -1;
}

int net_start_ap(const uint8_t mac[6], uint32_t ip, uint32_t mask, int (*send)(const uint8_t[6], uint16_t, const uint8_t *, unsigned), void (*poll)(void))
{
    lwip_init();
    ip4_addr_t a, m, z = { 0 };
    ip4_addr_set_u32(&a, ip); ip4_addr_set_u32(&m, mask);
    if (!netif_add(&nif, &a, &m, &z, 0, if_init, ethernet_input)) return -1;
    memcpy(nif.hwaddr, mac, 6);
    netif_set_default(&nif);
    netif_set_up(&nif);
    wifi_eth_rx = eth_in; ap_send = send; ap_poll = poll;
    return 0;
}

void net_poll(void) { if (ap_poll) ap_poll(); else wifi_poll(); sys_check_timeouts(); }

int net_ip(uint32_t *ip, uint32_t *gw)
{
    if (!dhcp_supplied_address(&nif)) return -1;
    if (ip) *ip = ip4_addr_get_u32(netif_ip4_addr(&nif));
    if (gw) *gw = ip4_addr_get_u32(netif_ip4_gw(&nif));
    return 0;
}

struct netif *net_netif(void) { return &nif; }

/* --- DNS (lwIP's resolver; the server comes from DHCP) and SNTP --- */
#include "lwip/dns.h"
#include "lwip/udp.h"
#include <sys/time.h>
#include "sys.h"

static volatile int dns_state; static uint32_t dns_addr;      /* 0 waiting, 1 found, -1 failed */
static void dns_cb(const char *name, const ip_addr_t *a, void *arg) { (void)name; (void)arg; if (a) { dns_addr = ip4_addr_get_u32(ip_2_ip4(a)); dns_state = 1; } else dns_state = -1; }
int net_dns(const char *host, uint32_t *ip, uint32_t timeout_ms)
{
    ip_addr_t a; dns_state = 0;
    err_t e = dns_gethostbyname(host, &a, dns_cb, 0);
    if (e == ERR_OK) { *ip = ip4_addr_get_u32(ip_2_ip4(&a)); return 0; }      /* a literal address or a cached name */
    if (e != ERR_INPROGRESS) return -1;
    uint32_t t0 = time_ms();
    while (!dns_state && time_ms() - t0 < timeout_ms) net_poll();
    if (dns_state != 1) return dns_state ? -1 : -2;
    *ip = dns_addr; return 0;
}

/* SNTP (RFC 4330) client: one request, the answer must come from the server's port 123, be a server-mode reply (mode 4) with a stratum
 * (0 = kiss of death) and echo our transmit timestamp (stricter than lwIP's default SNTP_CHECK_RESPONSE 0 that IDF uses: a spoofed
 * reply needs our timestamp). The clock is set to the server's transmit time plus half the round trip (IDF / lwIP: no compensation). */
static volatile int sntp_state; static uint8_t sntp_tx[8]; static int64_t sntp_us; static uint32_t sntp_t0, sntp_rtt;
static uint32_t sntp_server;
static void sntp_rx(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *from, u16_t port)
{
    (void)arg; (void)pcb;
    uint8_t r[48];
    if (sntp_state == 0 && port == 123 && ip4_addr_get_u32(ip_2_ip4(from)) == sntp_server && p->tot_len >= 48 && pbuf_copy_partial(p, r, 48, 0) == 48
        && (r[0] & 7) == 4 && r[1] && !memcmp(r + 24, sntp_tx, 8)) {
        uint32_t sec = (uint32_t)r[40] << 24 | r[41] << 16 | r[42] << 8 | r[43], fr = (uint32_t)r[44] << 24 | r[45] << 16 | r[46] << 8 | r[47];
        sntp_rtt = (uint32_t)(time_us() - sntp_t0);
        sntp_us = ((int64_t)sec - 2208988800LL) * 1000000 + (int64_t)(((uint64_t)fr * 1000000) >> 32) + sntp_rtt / 2;
        sntp_state = 1;
    } else if (sntp_state == 0 && port == 123 && p->tot_len >= 2 && pbuf_get_at(p, 1) == 0 && ip4_addr_get_u32(ip_2_ip4(from)) == sntp_server) sntp_state = -1;   /* kiss of death */
    pbuf_free(p);
}
int net_sntp(uint32_t server, uint32_t timeout_ms, uint32_t *rtt_us)
{
    struct udp_pcb *u = udp_new(); if (!u) return -1;
    udp_recv(u, sntp_rx, 0); sntp_server = server; sntp_state = 0;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 48, PBUF_RAM); if (!p) { udp_remove(u); return -1; }
    uint8_t *q = p->payload; memset(q, 0, 48); q[0] = 0x23;                     /* LI 0, VN 4, mode 3 client */
    uint64_t t = time_us(); for (int i = 0; i < 8; i++) sntp_tx[i] = (uint8_t)(t >> (56 - 8 * i));   /* a nonce in the transmit timestamp (RFC 4330 allows any value) */
    memcpy(q + 40, sntp_tx, 8);
    ip_addr_t a; ip_addr_set_ip4_u32(&a, server);
    sntp_t0 = (uint32_t)time_us(); err_t e = udp_sendto(u, p, &a, 123); pbuf_free(p);
    uint32_t t0 = time_ms();
    while (!e && !sntp_state && time_ms() - t0 < timeout_ms) net_poll();
    udp_remove(u);
    if (sntp_state != 1) return e ? -1 : sntp_state ? -3 : -2;
    struct timeval tv = { .tv_sec = (time_t)(sntp_us / 1000000), .tv_usec = (suseconds_t)(sntp_us % 1000000) }; settimeofday(&tv, 0);
    if (rtt_us) *rtt_us = sntp_rtt;
    return 0;
}
