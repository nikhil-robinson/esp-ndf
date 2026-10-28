#pragma GCC diagnostic ignored "-Wunused-parameter"      /* lwIP callback signatures */
#include <string.h>
#include "dhcpd.h"
#include "sys.h"
#include "lwip/udp.h"

#ifndef NDF_NET_DHCP_POOL
#define NDF_NET_DHCP_POOL 4
#endif
#define LEASE_S   3600u
#define OFFER_MS  10000u                           /* an offered address is held this long */

enum { DISCOVER = 1, OFFER, REQUEST, DECLINE, ACK, NAK, RELEASE };
static struct lease { uint8_t mac[6], state; uint32_t t; } L[NDF_NET_DHCP_POOL];       /* state 0 free, 1 offered, 2 bound */
static struct udp_pcb *pcb; static struct netif *nif; static uint32_t base, pool;

static uint32_t ip_of(unsigned i) { return lwip_htonl(lwip_ntohl(base) + i); }
static struct lease *by_mac(const uint8_t *mac) { for (unsigned i = 0; i < pool; i++) if (L[i].state && !memcmp(L[i].mac, mac, 6)) return &L[i]; return 0; }
static struct lease *take(const uint8_t *mac)
{
    struct lease *l = by_mac(mac); if (l) return l;
    uint32_t now = time_ms();
    for (unsigned i = 0; i < pool; i++) if (!L[i].state || (L[i].state == 1 && now - L[i].t > OFFER_MS) || (L[i].state == 2 && now - L[i].t > LEASE_S * 1000u)) { memset(&L[i], 0, sizeof L[i]); memcpy(L[i].mac, mac, 6); return &L[i]; }
    return 0;
}

static void reply(const uint8_t *req, uint8_t type, uint32_t yiaddr)
{
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 300, PBUF_RAM); if (!p) return;
    uint8_t *b = p->payload; memset(b, 0, 300);
    b[0] = 2; b[1] = 1; b[2] = 6; memcpy(b + 4, req + 4, 4); memcpy(b + 10, req + 10, 2);     /* BOOTREPLY, ethernet, xid, flags */
    memcpy(b + 16, &yiaddr, 4); uint32_t me = ip4_addr_get_u32(netif_ip4_addr(nif)); memcpy(b + 20, &me, 4);
    memcpy(b + 28, req + 28, 16);
    b[236] = 0x63; b[237] = 0x82; b[238] = 0x53; b[239] = 0x63;
    uint8_t *o = b + 240; uint32_t mask = ip4_addr_get_u32(netif_ip4_netmask(nif)), lease = lwip_htonl(LEASE_S);
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 54; *o++ = 4; memcpy(o, &me, 4); o += 4;
    if (type != NAK) {
        *o++ = 51; *o++ = 4; memcpy(o, &lease, 4); o += 4;
        *o++ = 1; *o++ = 4; memcpy(o, &mask, 4); o += 4;
        *o++ = 3; *o++ = 4; memcpy(o, &me, 4); o += 4;
        *o++ = 6; *o++ = 4; memcpy(o, &me, 4); o += 4;
    }
    *o++ = 255;
    pbuf_realloc(p, (u16_t)(o - b));
    udp_sendto_if(pcb, p, IP_ADDR_BROADCAST, 68, nif);
    pbuf_free(p);
}

static void rx(void *arg, struct udp_pcb *pc, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    uint8_t b[320];
    unsigned n = p->tot_len < sizeof b ? p->tot_len : sizeof b;
    pbuf_copy_partial(p, b, (u16_t)n, 0); pbuf_free(p);
    if (n < 241 || b[0] != 1 || b[1] != 1 || b[2] != 6 || b[236] != 0x63 || b[237] != 0x82 || b[238] != 0x53 || b[239] != 0x63) return;
    uint8_t type = 0; uint32_t want = 0, srv = 0;
    for (unsigned i = 240; i + 1 < n && b[i] != 255; ) {              /* options: every length checked */
        if (b[i] == 0) { i++; continue; }
        unsigned l = b[i + 1]; if (i + 2 + l > n) return;
        if (b[i] == 53 && l == 1) type = b[i + 2];
        else if (b[i] == 50 && l == 4) memcpy(&want, b + i + 2, 4);
        else if (b[i] == 54 && l == 4) memcpy(&srv, b + i + 2, 4);
        i += 2 + l;
    }
    uint32_t me = ip4_addr_get_u32(netif_ip4_addr(nif));
    struct lease *l;
    switch (type) {
    case DISCOVER:
        if ((l = take(b + 28))) { l->state = 1; l->t = time_ms(); reply(b, OFFER, ip_of((unsigned)(l - L))); }
        break;
    case REQUEST:
        if (srv && srv != me) { if ((l = by_mac(b + 28)) && l->state == 1) memset(l, 0, sizeof *l); break; }      /* the client took another server's offer */
        if (!want) memcpy(&want, b + 12, 4);                          /* renewal: ciaddr */
        if ((l = by_mac(b + 28)) && want == ip_of((unsigned)(l - L))) { l->state = 2; l->t = time_ms(); reply(b, ACK, want); }
        else reply(b, NAK, 0);
        break;
    case DECLINE: case RELEASE:
        if ((l = by_mac(b + 28))) memset(l, 0, sizeof *l);
        break;
    default: break;
    }
}

int dhcpd_start(struct netif *n, uint32_t first_ip, unsigned count)
{
    dhcpd_stop();
    if (!count || count > NDF_NET_DHCP_POOL) count = NDF_NET_DHCP_POOL;
    nif = n; base = first_ip; pool = count; memset(L, 0, sizeof L);
    pcb = udp_new(); if (!pcb) return -1;
    ip_set_option(pcb, SOF_BROADCAST);
    if (udp_bind(pcb, IP_ADDR_ANY, 67)) { udp_remove(pcb); pcb = 0; return -1; }
    udp_recv(pcb, rx, 0);
    return 0;
}

void dhcpd_stop(void) { if (pcb) { udp_remove(pcb); pcb = 0; } }

unsigned dhcpd_leases(uint32_t *ips, uint8_t macs[][6], unsigned max)
{
    unsigned k = 0;
    for (unsigned i = 0; i < pool && k < max; i++) if (L[i].state == 2) { if (ips) ips[k] = ip_of(i); if (macs) memcpy(macs[k], L[i].mac, 6); k++; }
    return k;
}
