/* ip_unit: lwIP on the Wi-Fi station, line commands driven by tools/wl/test_ip.py.
 *   up <ssid> | ping <a.b.c.d> <n> | udp <a.b.c.d> <port> <n> | tcp <a.b.c.d> <port> <bytes> | stats */
#pragma GCC diagnostic ignored "-Wunused-parameter"      /* lwIP callback signatures */
#include "console.h"
#include "sys.h"
#include "rf.h"
#include "wifi.h"
#include "net.h"
#include "wifi_mac.h"
#include "lwip/raw.h"
#include "lwip/dhcp.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/stats.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t my[6] = { 0x02, 0x4e, 0x44, 0x46, 0x00, 0x02 };
static ip4_addr_t peer;
static int parse_ip(const char *s, ip4_addr_t *a)
{
    uint8_t b[4]; for (int i = 0; i < 4; i++) { char *e; b[i] = (uint8_t)strtoul(s, &e, 10); if (e == s) return -1; s = e + (*e == '.'); }
    IP4_ADDR(a, b[0], b[1], b[2], b[3]); return 0;
}
static void pump_ms(uint32_t ms) { uint32_t t0 = time_ms(); while (time_ms() - t0 < ms) net_poll(); }

/* --- ping --- */
static unsigned ping_rx, ping_seq; static uint32_t ping_t0, ping_sum;
static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    if (p->tot_len >= 28 && ((uint8_t *)p->payload)[20] == ICMP_ER && ip4_addr_cmp(ip_2_ip4(addr), &peer)) {
        const struct icmp_echo_hdr *e = (const struct icmp_echo_hdr *)((uint8_t *)p->payload + 20);
        if (lwip_ntohs(e->seqno) == ping_seq) { ping_rx++; ping_sum += (uint32_t)(time_us() - ping_t0); pbuf_free(p); return 1; }
    }
    return 0;
}
static void do_ping(unsigned n)
{
    struct raw_pcb *r = raw_new(IP_PROTO_ICMP); raw_recv(r, ping_recv, 0); raw_bind(r, IP_ADDR_ANY);
    ping_rx = ping_sum = 0;
    for (unsigned i = 0; i < n; i++) {
        struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + 32, PBUF_RAM); if (!p) continue;
        struct icmp_echo_hdr *e = p->payload; ICMPH_TYPE_SET(e, ICMP_ECHO); ICMPH_CODE_SET(e, 0); e->chksum = 0; e->id = lwip_htons(0x4e44); e->seqno = lwip_htons((u16_t)(++ping_seq));
        memset((uint8_t *)p->payload + sizeof *e, 'a' + (int)(i & 15), 32);
        e->chksum = inet_chksum(p->payload, p->len);
        ping_t0 = time_us(); raw_sendto(r, p, (ip_addr_t *)&peer); pbuf_free(p);
        uint32_t t0 = time_ms(); unsigned before = ping_rx;
        while (ping_rx == before && time_ms() - t0 < 800) net_poll();
        pump_ms(50);
    }
    raw_remove(r);
    printf("ping sent %u recv %u avg_us %lu\n", n, ping_rx, (unsigned long)(ping_rx ? ping_sum / ping_rx : 0));
}

/* --- udp echo --- */
static unsigned udp_ok, udp_bad, udp_lost, udp_len, udp_got;
static unsigned udp_seq;
static uint8_t udp_pat(unsigned i, unsigned k) { return (uint8_t)(i * 31 + k); }
static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    unsigned want = udp_len; int good = p->tot_len == want;
    for (unsigned k = 0; good && k < want; k++) if (pbuf_get_at(p, (u16_t)k) != udp_pat(udp_seq, k)) good = 0;
    if (good) udp_ok++; else udp_bad++;
    udp_got = 1; pbuf_free(p);
}
static void do_udp(unsigned port, unsigned n)
{
    struct udp_pcb *u = udp_new(); udp_recv(u, udp_recv_cb, 0); udp_bind(u, IP_ADDR_ANY, 0);
    udp_ok = udp_bad = udp_lost = 0;
    for (unsigned i = 0; i < n; i++) {
        udp_seq = i; udp_len = 1 + (i * 37) % 1400; udp_got = 0;
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)udp_len, PBUF_RAM); if (!p) { udp_lost++; continue; }
        for (unsigned k = 0; k < udp_len; k++) ((uint8_t *)p->payload)[k] = udp_pat(i, k);
        udp_sendto(u, p, (ip_addr_t *)&peer, (u16_t)port); pbuf_free(p);
        uint32_t t0 = time_ms(); while (!udp_got && time_ms() - t0 < 500) net_poll();
        if (!udp_got) udp_lost++;
    }
    udp_remove(u);
    printf("udp sent %u ok %u lost %u corrupt %u\n", n, udp_ok, udp_lost, udp_bad);
}

/* --- tcp echo --- */
static struct tcp_pcb *tp; static uint32_t t_total, t_sent, t_rcvd, t_bad; static int t_state;     /* 0 connecting, 1 up, 2 closed by peer, 3 error */
static uint8_t tcp_pat(uint32_t i) { return (uint8_t)(i * 7 + (i >> 8)); }
static err_t t_recv(void *a, struct tcp_pcb *p, struct pbuf *q, err_t e)
{
    if (!q) { t_state = 2; return ERR_OK; }
    for (struct pbuf *b = q; b; b = b->next) for (u16_t k = 0; k < b->len; k++) { if (((uint8_t *)b->payload)[k] != tcp_pat(t_rcvd)) t_bad++; t_rcvd++; }
    tcp_recved(p, q->tot_len); pbuf_free(q); return ERR_OK;
}
static err_t t_conn(void *a, struct tcp_pcb *p, err_t e) { t_state = e ? 3 : 1; return ERR_OK; }
static void t_err(void *a, err_t e) { t_state = 3; tp = 0; }
static void do_tcp(unsigned port, uint32_t bytes)
{
    t_total = bytes; t_sent = t_rcvd = t_bad = 0; t_state = 0;
    tp = tcp_new(); if (!tp) { printf("tcp no pcb\n"); return; }
    tcp_recv(tp, t_recv); tcp_err(tp, t_err); tcp_nagle_disable(tp);     /* a bulk sender: no 200 ms wait on the last partial segment */
    if (tcp_connect(tp, &peer, (u16_t)port, t_conn)) { printf("tcp connect refused\n"); return; }
    uint32_t t0 = time_ms(), t1 = 0;
    while (t_state == 0 && time_ms() - t0 < 4000) net_poll();
    if (t_state == 1) t1 = time_ms();
    static uint8_t chunk[1460];
    while (t_state == 1 && t_rcvd < bytes && time_ms() - t1 < 120000) {
        while (t_sent < bytes) {
            u16_t room = tcp_sndbuf(tp); uint32_t n = bytes - t_sent; if (n > sizeof chunk) n = sizeof chunk; if (n > room) n = room;
            if (!n) break;
            for (uint32_t k = 0; k < n; k++) chunk[k] = tcp_pat(t_sent + k);
            if (tcp_write(tp, chunk, (u16_t)n, TCP_WRITE_FLAG_COPY)) break;
            t_sent += n;
        }
        tcp_output(tp); net_poll();
    }
    uint32_t ms = time_ms() - t1;
    if (tp) { tcp_arg(tp, 0); tcp_recv(tp, 0); tcp_err(tp, 0); if (tcp_close(tp)) tcp_abort(tp); tp = 0; }
    pump_ms(200);
    printf("tcp state %d sent %lu rcvd %lu bad %lu in %lu ms\n", t_state, (unsigned long)t_sent, (unsigned long)t_rcvd, (unsigned long)t_bad, (unsigned long)ms);
}

int main(void)
{
    delay_ms(300);
    printf("ip_unit ready open %d\n", rf_open());
    static char line[100]; int k = 0, net_on = 0;
    for (;;) {
        net_poll();
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch != '\n') { if (k < (int)sizeof line - 1) line[k++] = (char)ch; continue; }
        line[k] = 0; k = 0;
        char *v[4]; int n = 0;
        for (char *p = line; *p && n < 4; ) { while (*p == ' ') *p++ = 0; if (!*p) break; v[n++] = p; while (*p && *p != ' ') p++; }
        if (!n) { printf("ok\n"); continue; }
        if (!strcmp(v[0], "up") && n > 1) {
            uint32_t t0 = time_ms(); int rc = wifi_init(my); if (!rc) rc = wifi_sta_join(v[1], n > 2 ? v[2] : 0, 5000);
            uint32_t tj = time_ms() - t0; uint32_t ip = 0, gw = 0;
            if (!rc) rc = net_start(my);
            if (!rc) net_on = 1;
            while (!rc && net_ip(&ip, &gw) && time_ms() - t0 < 8000) net_poll();
            ip4_addr_t a = { ip }, g = { gw }; char ab[16], gb[16];
            printf("up rc %d join_ms %lu total_ms %lu ip %s gw %s\n", rc, (unsigned long)tj, (unsigned long)(time_ms() - t0), ip ? ip4addr_ntoa_r(&a, ab, sizeof ab) : "-", gw ? ip4addr_ntoa_r(&g, gb, sizeof gb) : "-");
        } else if (!net_on && (!strcmp(v[0], "ping") || !strcmp(v[0], "udp") || !strcmp(v[0], "tcp") || !strcmp(v[0], "stats"))) printf("not up\n");     /* lwIP is started by a successful up */
        else if (!strcmp(v[0], "ping") && n > 2 && !parse_ip(v[1], &peer)) do_ping((unsigned)atoi(v[2]));
        else if (!strcmp(v[0], "udp") && n > 3 && !parse_ip(v[1], &peer)) do_udp((unsigned)atoi(v[2]), (unsigned)atoi(v[3]));
        else if (!strcmp(v[0], "tcp") && n > 3 && !parse_ip(v[1], &peer)) do_tcp((unsigned)atoi(v[2]), (uint32_t)atoi(v[3]));
        else if (!strcmp(v[0], "rate") && n > 1) { wifi_set_data_rate(strtoul(v[1], 0, 0)); printf("rate %s\n", v[1]); }
        else if (!strcmp(v[0], "wr") && n > 2) { *(volatile uint32_t *)strtoul(v[1], 0, 0) = strtoul(v[2], 0, 0); printf("wr\n"); }
        else if (!strcmp(v[0], "static") && n > 1) {                  /* static <ip>: after "up": drop DHCP, use this address (netmask /24, gateway .1) */
            ip4_addr_t a, m, g; parse_ip(v[1], &a); IP4_ADDR(&m, 255, 255, 255, 0); IP4_ADDR(&g, 192, 168, 4, 1);
            dhcp_stop(net_netif()); netif_set_addr(net_netif(), &a, &m, &g); printf("static %s\n", v[1]);
        } else if (!strcmp(v[0], "rawarp")) {                           /* the same raw ARP request as sta_unit, bypassing lwIP */
            uint8_t a[28] = { 0, 1, 8, 0, 6, 4, 0, 1 }, bc[6] = { 255, 255, 255, 255, 255, 255 };
            memcpy(a + 8, my, 6); a[14] = 192; a[15] = 168; a[16] = 4; a[17] = 50; a[24] = 192; a[25] = 168; a[26] = 4; a[27] = 1;
            printf("rawarp tx %d\n", wifi_send_eth(bc, 0x0806, a, 28)); pump_ms(600);
        } else if (!strcmp(v[0], "rd") && n > 2) { uint32_t a = strtoul(v[1], 0, 0), c = strtoul(v[2], 0, 0); for (uint32_t i = 0; i < c; i++) printf("%08x %08x\n", (unsigned)(a + 4 * i), (unsigned)*(volatile uint32_t *)(a + 4 * i));
                } else if (!strcmp(v[0], "hw")) { const uint8_t *h = net_netif()->hwaddr; printf("hw %02x:%02x:%02x:%02x:%02x:%02x len %d\n", h[0], h[1], h[2], h[3], h[4], h[5], net_netif()->hwaddr_len);
        } else if (!strcmp(v[0], "mac")) { uint32_t a, b, c, d; wifi_mac_stats(&a, &b, &c, &d); extern uint32_t n_drop_mine, n_mine, n_retry; printf("mac to-me %lu retries %lu mine-dropped %lu rx %lu noise %lu tx %lu txfail %lu\n", (unsigned long)n_mine, (unsigned long)n_retry, (unsigned long)n_drop_mine, (unsigned long)a, (unsigned long)b, (unsigned long)c, (unsigned long)d); }
        else if (!strcmp(v[0], "i2crd") && n > 1) { extern uint8_t rf_regi2c_read(uint8_t, uint8_t); uint8_t b = (uint8_t)strtoul(v[1], 0, 0); printf("i2c %02x", b); for (unsigned r = 0; r < 32; r++) printf(" %02x", rf_regi2c_read(b, (uint8_t)r)); printf("\n"); }
        else if (!strcmp(v[0], "link")) { uint32_t t0 = time_ms(); while (time_ms() - t0 < (n > 1 ? (uint32_t)atoi(v[1]) : 0)) net_poll(); printf("link %d\n", wifi_sta_up()); }   /* link [ms]: poll for ms, then the station state */
        else if (!strcmp(v[0], "stats"))
            printf("stats link xmit %u rx %u drop %u memerr %u ip drop %u tcp xmit %u mem used %u max %u pbuf pool err %u\n", lwip_stats.link.xmit, lwip_stats.link.recv, lwip_stats.link.drop, lwip_stats.link.memerr, lwip_stats.ip.drop, lwip_stats.tcp.xmit,
                   (unsigned)lwip_stats.mem.used, (unsigned)lwip_stats.mem.max, (unsigned)lwip_stats.memp[MEMP_PBUF_POOL]->err);
        else printf("?\n");
        printf("ok\n");
    }
}
