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
static uint8_t txb[1600];

static err_t wifi_output(struct netif *n, struct pbuf *p)
{
    (void)n;
    if (p->tot_len < 14 || p->tot_len > sizeof txb) return ERR_BUF;
    pbuf_copy_partial(p, txb, p->tot_len, 0);                   /* one flat frame: 14-byte Ethernet header + payload */
    LINK_STATS_INC(link.xmit);
    return wifi_send_eth(txb, (uint16_t)(txb[12] << 8 | txb[13]), txb + 14, p->tot_len - 14) ? ERR_IF : ERR_OK;   /* destination = first 6 bytes of txb */
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
    lwip_init();
    ip4_addr_t z = { 0 };
    if (!netif_add(&nif, &z, &z, &z, 0, if_init, ethernet_input)) return -1;
    memcpy(nif.hwaddr, mac, 6);
    netif_set_default(&nif);
    netif_set_up(&nif);
    wifi_eth_rx = eth_in;
    return dhcp_start(&nif) == ERR_OK ? 0 : -1;
}

void net_poll(void) { wifi_poll(); sys_check_timeouts(); }

int net_ip(uint32_t *ip, uint32_t *gw)
{
    if (!dhcp_supplied_address(&nif)) return -1;
    if (ip) *ip = ip4_addr_get_u32(netif_ip4_addr(&nif));
    if (gw) *gw = ip4_addr_get_u32(netif_ip4_gw(&nif));
    return 0;
}

struct netif *net_netif(void) { return &nif; }
