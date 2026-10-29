/* https_unit: line commands over the console, driven by tools/wl/test_https.py.
 *   up <ssid> | serve <port> | get <https-url> <host|-> [other] (other: trust the unrelated CA) | memused */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "console.h"
#include "sys.h"
#include "rf.h"
#include "wifi.h"
#include "net.h"
#include "https.h"
#include "certs.h"
#include "lwip/stats.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t my[6] = { 0x02, 0x4e, 0x44, 0x46, 0x00, 0x02 };
static const tls_server_cfg_t srv = { tls_server_cert, sizeof tls_server_cert, tls_server_priv };
static int h_hello(const http_req_t *q, http_resp_t *r) { r->body = (const uint8_t *)"hello"; r->len = 5; return 0; }
static int h_big(const http_req_t *q, http_resp_t *r) { extern const char _flash_rodata_start[]; r->body = (const uint8_t *)_flash_rodata_start; r->len = 65536; r->ctype = "application/octet-stream"; return 0; }

int main(void)
{
    delay_ms(300);
    printf("https_unit ready open %d\n", rf_open());
    static char line[120]; int k = 0;
    for (;;) {
        net_poll();
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch != '\n') { if (k < (int)sizeof line - 1) line[k++] = (char)ch; continue; }
        line[k] = 0; k = 0;
        char *v[5]; int n = 0;
        for (char *p = line; *p && n < 5; ) { while (*p == ' ') *p++ = 0; if (!*p) break; v[n++] = p; while (*p && *p != ' ') p++; }
        if (!n) { printf("ok\n"); continue; }
        if (!strcmp(v[0], "up") && n > 1) {
            uint32_t t0 = time_ms(); int rc = wifi_init(my); if (!rc) rc = wifi_sta_join(v[1], 0, 5000);
            uint32_t ip = 0; if (!rc) rc = net_start(my);
            while (!rc && net_ip(&ip, 0) && time_ms() - t0 < 15000) net_poll();
            char b[16]; ip4_addr_t a = { ip };
            printf("up rc %d ip %s\n", rc, ip ? ip4addr_ntoa_r(&a, b, sizeof b) : "-");
        } else if (!strcmp(v[0], "serve") && n > 1) {
            http_route("GET", "/hello", h_hello); http_route("GET", "/big", h_big);
            printf("serve %d\n", https_server_start((uint16_t)atoi(v[1]), &srv));
        } else if (!strcmp(v[0], "get") && n > 2) {
            static uint8_t body[600]; int st = 0; uint32_t t0 = time_ms();
            tls_client_cfg_t cfg = { .ca_pub = n > 3 ? tls_other_ca_pub : tls_ca_pub, .host = strcmp(v[2], "-") ? v[2] : 0 };
            int len = https_get(v[1], &cfg, body, sizeof body, &st); unsigned bad = 0;
            for (int i = 0; i < len && i < (int)sizeof body && strstr(v[1], "/big"); i++) if (body[i] != 'x') bad++;
            printf("get status %d bytes %d bad %u in %lu ms\n", st, len, bad, (unsigned long)(time_ms() - t0));
        } else if (!strcmp(v[0], "memused")) printf("memused %u\n", (unsigned)lwip_stats.mem.used);
        else printf("?\n");
        printf("ok\n");
    }
}
