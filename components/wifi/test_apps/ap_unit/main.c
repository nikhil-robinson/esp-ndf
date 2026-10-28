#pragma GCC diagnostic ignored "-Wunused-parameter"      /* lwIP callback signatures */
/* ap_unit: line commands over the console, driven by tools/wl/test_ap.py.
 *   start <ssid> [pass] | stop | stas | echo <port> (TCP echo server) | stats | poll <ms> | max <n> (limit for the next start) */
#include "console.h"
#include "sys.h"
#include "rf.h"
#include "wifi.h"
#include "net.h"
#include "dhcpd.h"
#include "lwip/tcp.h"
#include "lwip/stats.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t my[6] = { 0x02, 0x4e, 0x44, 0x46, 0x00, 0x01 };
static unsigned max_sta = 4;

static err_t e_recv(void *a, struct tcp_pcb *p, struct pbuf *q, err_t e)
{
    if (!q) { tcp_close(p); return ERR_OK; }
    for (struct pbuf *b = q; b; b = b->next) tcp_write(p, b->payload, b->len, TCP_WRITE_FLAG_COPY);   /* window = send buffer, so it fits */
    tcp_output(p); tcp_recved(p, q->tot_len); pbuf_free(q);
    return ERR_OK;
}
static err_t e_accept(void *a, struct tcp_pcb *p, err_t e) { tcp_recv(p, e_recv); return ERR_OK; }

int main(void)
{
    delay_ms(300);
    printf("ap_unit ready open %d\n", rf_open());
    static char line[100]; int k = 0; int started = 0, net_up = 0;
    for (;;) {
        if (started) net_poll();
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch != '\n') { if (k < (int)sizeof line - 1) line[k++] = (char)ch; continue; }
        line[k] = 0; k = 0;
        char *v[4]; int n = 0;
        for (char *p = line; *p && n < 4; ) { while (*p == ' ') *p++ = 0; if (!*p) break; v[n++] = p; while (*p && *p != ' ') p++; }
        if (!n) { printf("ok\n"); continue; }
        if (!strcmp(v[0], "start") && n > 1) {
            int rc = wifi_init(my);
            if (!rc) rc = wifi_ap_start(v[1], n > 2 ? v[2] : 0, 6, max_sta);
            if (!rc && !net_up) { rc = net_start_ap(my, lwip_htonl(0xC0A80401), lwip_htonl(0xFFFFFF00), wifi_ap_send_eth, wifi_ap_poll); net_up = !rc; }
            if (!rc) rc = dhcpd_start(net_netif(), lwip_htonl(0xC0A80402), 4);
            started = !rc; printf("start %d\n", rc);
        } else if (!strcmp(v[0], "stop")) { wifi_ap_stop(); dhcpd_stop(); started = 0; printf("stop\n"); }
        else if (!strcmp(v[0], "max") && n > 1) { max_sta = (unsigned)atoi(v[1]); printf("max %u\n", max_sta); }
        else if (!strcmp(v[0], "echo") && n > 1) {
            struct tcp_pcb *l = tcp_new(); tcp_bind(l, IP_ADDR_ANY, (u16_t)atoi(v[1])); l = tcp_listen(l); tcp_accept(l, e_accept); printf("echo %s\n", v[1]);
        } else if (!strcmp(v[0], "stas")) {
            static uint8_t m[8][6]; unsigned c = wifi_ap_stations(m, 8);
            printf("stas %u\n", c);
            for (unsigned i = 0; i < c; i++) printf("sta %02x:%02x:%02x:%02x:%02x:%02x\n", m[i][0], m[i][1], m[i][2], m[i][3], m[i][4], m[i][5]);
            static uint32_t ip[8]; static uint8_t lm[8][6]; unsigned d = dhcpd_leases(ip, lm, 8);
            printf("leases %u\n", d);
            for (unsigned i = 0; i < d; i++) printf("lease %02x:%02x:%02x:%02x:%02x:%02x %u.%u.%u.%u\n", lm[i][0], lm[i][1], lm[i][2], lm[i][3], lm[i][4], lm[i][5], (unsigned)(ip[i] & 255), (unsigned)(ip[i] >> 8 & 255), (unsigned)(ip[i] >> 16 & 255), (unsigned)(ip[i] >> 24));
        } else if (!strcmp(v[0], "rd") && n > 2) { uint32_t ad = strtoul(v[1], 0, 0), c = strtoul(v[2], 0, 0); for (uint32_t i = 0; i < c; i++) printf("%08x %08x\n", (unsigned)(ad + 4 * i), (unsigned)*(volatile uint32_t *)(ad + 4 * i));
        } else if (!strcmp(v[0], "wr") && n > 2) { *(volatile uint32_t *)strtoul(v[1], 0, 0) = strtoul(v[2], 0, 0); printf("wr\n");
        } else if (!strcmp(v[0], "apstat")) { uint32_t au, as, re, ht, be; unsigned us; wifi_ap_stats(&au, &as, &re, &ht, &be, &us); printf("apstat auth %lu assoc %lu refused %lu hs_timeout %lu beacons %lu used %u\n", (unsigned long)au, (unsigned long)as, (unsigned long)re, (unsigned long)ht, (unsigned long)be, us);
        } else if (!strcmp(v[0], "stats"))
            printf("stats link xmit %u rx %u drop %u memerr %u mem used %u max %u\n", lwip_stats.link.xmit, lwip_stats.link.recv, lwip_stats.link.drop, lwip_stats.link.memerr, (unsigned)lwip_stats.mem.used, (unsigned)lwip_stats.mem.max);
        else if (!strcmp(v[0], "poll") && n > 1) { uint32_t t0 = time_ms(), ms = (uint32_t)atoi(v[1]); while (time_ms() - t0 < ms) if (started) net_poll(); printf("polled\n"); }
        printf("ok\n");
    }
}
