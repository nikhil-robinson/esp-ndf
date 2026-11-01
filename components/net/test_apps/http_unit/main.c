/* http_unit: line commands over the console, driven by tools/wl/test_http.py.
 *   up <ssid> | serve <port> | get <url> | memused */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "console.h"
#include "sys.h"
#include "rf.h"
#include "wifi.h"
#include "wifi_mac.h"
#include "net.h"
#include "http.h"
#include "ws.h"
#include "mqtt.h"
#include "lwip/stats.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t my[6] = { 0x02, 0x4e, 0x44, 0x46, 0x00, 0x02 };
extern const char _flash_rodata_start[];
/* WebSocket: the server echoes every message; the client counts echoes of the expected size */
static void ws_echo(int id, int op, const uint8_t *p, unsigned n) { if (op != WS_CLOSE) ws_send(id, op, p, n); }
static volatile unsigned wc_got, wc_len; static volatile int wc_closed;
static void ws_cl(int id, int op, const uint8_t *p, unsigned n) { if (op == WS_CLOSE) wc_closed = 1; else { wc_len = n; wc_got++; } }
/* MQTT: count messages, keep the last one */
static volatile unsigned mq_got, mq_len; static char mq_topic[64], mq_last[64]; static int mq_retain;
static void mq_cb(const char *t, unsigned tl, const uint8_t *p, unsigned n, int qos, int retain)
{ mq_got++; mq_len = n; mq_retain = retain; unsigned k = tl < 63 ? tl : 63; memcpy(mq_topic, t, k); mq_topic[k] = 0; k = n < 63 ? n : 63; memcpy(mq_last, p, k); mq_last[k] = 0; }
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
        } else if (!strcmp(v[0], "mqconn") && n > 1) {                 /* mqconn <url> [user pass [id [will_topic]]] */
            static char u[32], pw[32], id[24], wt[32]; mqtt_cfg_t c = { 0 };
            if (n > 3) { strncpy(u, v[2], 31); strncpy(pw, v[3], 31); c.user = u; c.pass = pw; }
            uint32_t t0 = time_ms(); int rc = mqtt_connect(v[1], &c, mq_cb, 5000);
            printf("mqconn rc %d ms %lu\n", rc, (unsigned long)(time_ms() - t0)); (void)id; (void)wt;
        } else if (!strcmp(v[0], "mqwill") && n > 2) {                  /* mqwill <url> <will_topic>: connect with a will (QoS 1, "gone") as id "ndf-will" */
            static char wt[32]; strncpy(wt, v[2], 31); mqtt_cfg_t c = { .client_id = "ndf-will", .will_topic = wt, .will_msg = (const uint8_t *)"gone", .will_len = 4, .will_qos = 1 };
            printf("mqwill rc %d\n", mqtt_connect(v[1], &c, mq_cb, 5000));
        } else if (!strcmp(v[0], "mqsub") && n > 2) printf("mqsub %d\n", mqtt_subscribe(v[1], atoi(v[2]), 3000));
        else if (!strcmp(v[0], "mqunsub") && n > 1) printf("mqunsub %d\n", mqtt_unsubscribe(v[1], 3000));
        else if (!strcmp(v[0], "mqpub") && n > 3) {                     /* mqpub <topic> <msg> <qos> [retain: as a 4th word is not available, qos 2 = qos 0 retained] */
            int q = atoi(v[3]); int rc = mqtt_publish(v[1], v[2], (unsigned)strlen(v[2]), q == 2 ? 0 : q, q == 2);
            uint32_t t0 = time_ms(); while (mqtt_inflight() && time_ms() - t0 < 5000) net_poll();
            printf("mqpub rc %d acked %d\n", rc, !mqtt_inflight());
        } else if (!strcmp(v[0], "mqrecv") && n > 1) { uint32_t t0 = time_ms(), ms = (uint32_t)atoi(v[1]); while (time_ms() - t0 < ms) net_poll(); printf("mqrecv got %u topic %s msg %s retain %d conn %d\n", mq_got, mq_topic, mq_last, mq_retain, mqtt_connected()); mq_got = 0; }
        else if (!strcmp(v[0], "mqdisc")) { mqtt_disconnect(); printf("mqdisc\n"); }
        else if (!strcmp(v[0], "mqdrop")) { extern void mqtt_test_drop(void); mqtt_test_drop(); printf("mqdrop\n"); }
        else if (!strcmp(v[0], "mqbench") && n > 3) {                   /* mqbench <url> <n> <bytes|qos>: v[3] = bytes, qos from v[2] sign: as the peer's mqtt_bench */
            unsigned cnt = (unsigned)atoi(v[2]), sz = (unsigned)atoi(v[3]), ok = 0; int qos = cnt >= 1000 ? 1 : 0; if (qos) cnt -= 1000;   /* n >= 1000: QoS 1, n - 1000 messages */
            static uint8_t b[NDF_MQTT_BUF]; memset(b, 'm', sizeof b); uint32_t t0 = time_ms(); mq_got = 0;
            int rc = mqtt_connect(v[1], 0, mq_cb, 5000); uint32_t tc = time_ms();
            if (!rc) mqtt_subscribe("bench/ndf", qos, 3000);
            uint32_t ts = time_ms();
            for (unsigned i = 0; i < cnt && !rc; i++) { unsigned want = mq_got + 1; if (mqtt_publish("bench/ndf", b, sz, qos, 0)) break; uint32_t w = time_ms(); while ((mq_got < want || mqtt_inflight()) && time_ms() - w < 5000) net_poll(); if (mq_got >= want && mq_len == sz) ok++; }
            uint32_t td = time_ms(); mqtt_disconnect();
            printf("mqbench rc %d connect_ms %lu echoes %u of %u qos %d rt_ms %lu\n", rc, (unsigned long)(tc - t0), ok, cnt, qos, (unsigned long)(td - ts));
        } else if (!strcmp(v[0], "memused")) printf("memused %u\n", (unsigned)lwip_stats.mem.used);
        else if (!strcmp(v[0], "mac")) { uint32_t a, b, c, d; wifi_mac_stats(&a, &b, &c, &d); extern uint32_t n_drop_mine, n_mine, n_retry, n_retries_tx, n_noack, n_cwb40, n_agg, n_mine40, n_retry40, sta_n_amsdu, sta_n_sub, sta_n_bad, sta_n_dup, sta_n_addba;   /* MAC counters (diagnostic) */
            printf("mac to-me %lu retries %lu mine-dropped %lu rx %lu noise %lu tx %lu txfail %lu tx-retries %lu noack %lu cwb40 %lu to-me40 %lu retries40 %lu agg %lu amsdu %lu sub %lu bad %lu dup %lu addba %lu | tcp rx %u drop %u chkerr %u err %u ip chkerr %u drop %u link drop %u\n", (unsigned long)n_mine, (unsigned long)n_retry, (unsigned long)n_drop_mine, (unsigned long)a, (unsigned long)b,
                   (unsigned long)c, (unsigned long)d, (unsigned long)n_retries_tx, (unsigned long)n_noack, (unsigned long)n_cwb40, (unsigned long)n_mine40, (unsigned long)n_retry40, (unsigned long)n_agg, (unsigned long)sta_n_amsdu, (unsigned long)sta_n_sub, (unsigned long)sta_n_bad, (unsigned long)sta_n_dup, (unsigned long)sta_n_addba, lwip_stats.tcp.recv, lwip_stats.tcp.drop, lwip_stats.tcp.chkerr, lwip_stats.tcp.err, lwip_stats.ip.chkerr, lwip_stats.ip.drop, lwip_stats.link.drop); }
        else printf("?\n");
        printf("ok\n");
    }
}
