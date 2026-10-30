/* http_unit: line commands over the console, driven by tools/wl/test_http.py.
 *   up <ssid> | serve <port> | get <url> | memused */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "console.h"
#include "sys.h"
#include "rf.h"
#include "wifi.h"
#include "net.h"
#include "http.h"
#include "ws.h"
#include "lwip/stats.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t my[6] = { 0x02, 0x4e, 0x44, 0x46, 0x00, 0x02 };
extern const char _flash_rodata_start[];
/* WebSocket: the server echoes every message; the client counts echoes of the expected size */
static void ws_echo(int id, int op, const uint8_t *p, unsigned n) { if (op != WS_CLOSE) ws_send(id, op, p, n); }
static volatile unsigned wc_got, wc_len; static volatile int wc_closed;
static void ws_cl(int id, int op, const uint8_t *p, unsigned n) { if (op == WS_CLOSE) wc_closed = 1; else { wc_len = n; wc_got++; } }
static int h_hello(const http_req_t *q, http_resp_t *r) { r->body = (const uint8_t *)"hello"; r->len = 5; return 0; }
static int h_big(const http_req_t *q, http_resp_t *r) { r->body = (const uint8_t *)_flash_rodata_start; r->len = 65536; r->ctype = "application/octet-stream"; return 0; }   /* 64 KB of constant flash data */

int main(void)
{
    delay_ms(300);
    printf("http_unit ready open %d\n", rf_open());
    static char line[100]; int k = 0;
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
            uint32_t t0 = time_ms(); int rc = wifi_init(my); if (!rc) rc = wifi_sta_join(v[1], 0, 5000);
            uint32_t ip = 0; if (!rc) rc = net_start(my);
            while (!rc && net_ip(&ip, 0) && time_ms() - t0 < 15000) net_poll();
            char b[16]; ip4_addr_t a = { ip };
            printf("up rc %d ip %s\n", rc, ip ? ip4addr_ntoa_r(&a, b, sizeof b) : "-");
        } else if (!strcmp(v[0], "serve") && n > 1) {
            http_route("GET", "/hello", h_hello); http_route("GET", "/big", h_big);
            printf("serve %d\n", http_server_start((uint16_t)atoi(v[1])));
        } else if (!strcmp(v[0], "get") && n > 1) {
            static uint8_t body[600]; int st = 0; uint32_t t0 = time_ms();
            int len = http_get(v[1], body, sizeof body, &st); unsigned bad = 0;
            for (int i = 0; i < len && i < (int)sizeof body && strstr(v[1], "/big"); i++) if (body[i] != 'x') bad++;
            printf("get status %d bytes %d bad %u in %lu ms\n", st, len, bad, (unsigned long)(time_ms() - t0));
        } else if (!strcmp(v[0], "wsserve")) printf("wsserve %d\n", ws_route("/ws", ws_echo));
        else if (!strcmp(v[0], "wsget") && n > 3) {                    /* wsget <url> <n> <bytes>: as the peer's ws_get, plus a ping */
            unsigned cnt = (unsigned)atoi(v[2]), sz = (unsigned)atoi(v[3]), ok = 0; static uint8_t b[NDF_WS_MSG_MAX]; for (unsigned i = 0; i < sz && i < sizeof b; i++) b[i] = (uint8_t)i;
            uint32_t t0 = time_ms(); wc_got = 0; wc_closed = 0;
            int rc = ws_connect(v[1], ws_cl, 5000); uint32_t tc = time_ms();
            for (unsigned i = 0; i < cnt && !rc; i++) { unsigned want = wc_got + 1; if (ws_send(WS_CLIENT, WS_BIN, b, sz)) break; uint32_t w0 = time_ms(); while (wc_got < want && time_ms() - w0 < 5000) net_poll(); if (wc_got >= want && wc_len == sz) ok++; }
            uint32_t td = time_ms(); unsigned want = wc_got + 1; int text = 0;
            if (!rc && !ws_send(WS_CLIENT, WS_TEXT, "hello", 5)) { uint32_t w0 = time_ms(); while (wc_got < want && time_ms() - w0 < 5000) net_poll(); text = wc_got >= want && wc_len == 5; }
            int ping = !rc && !ws_send(WS_CLIENT, WS_PING, "p", 1);
            if (!rc) { ws_close(WS_CLIENT, 1000); uint32_t w0 = time_ms(); while (!wc_closed && time_ms() - w0 < 3000) net_poll(); }
            printf("wsget rc %d connect_ms %lu echoes %u of %u text %d ping %d closed %d echo_ms %lu\n", rc, (unsigned long)(tc - t0), ok, cnt, text, ping, wc_closed, (unsigned long)(td - tc));
        } else if (!strcmp(v[0], "memused")) printf("memused %u\n", (unsigned)lwip_stats.mem.used);
        else printf("?\n");
        printf("ok\n");
    }
}
