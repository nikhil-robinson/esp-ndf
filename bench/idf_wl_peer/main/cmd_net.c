/* Network commands of the peer (lwIP sockets): ping, UDP / TCP echo servers, TCP sender, iperf-like rate tests, HTTP. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_https_server.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "ping/ping_sock.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "../peer.h"

static SemaphoreHandle_t psem;
static volatile uint32_t p_ok, p_n, p_time_sum;
static void p_ok_cb(esp_ping_handle_t h, void *a) { uint32_t t; esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &t, sizeof t); p_ok++; p_time_sum += t; }
static void p_end_cb(esp_ping_handle_t h, void *a) { xSemaphoreGive(psem); }
static int cmd_ping(int argc, char **argv)                       /* ping <ip> <count> */
{
    if (argc < 3) return -1;
    if (!psem) psem = xSemaphoreCreateBinary();
    esp_ping_config_t c = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t ip; if (!ipaddr_aton(argv[1], &ip)) return -2;
    c.target_addr = ip; c.count = atoi(argv[2]); c.interval_ms = 50; c.timeout_ms = 1000;
    esp_ping_callbacks_t cb = { .on_ping_success = p_ok_cb, .on_ping_end = p_end_cb };
    p_ok = p_time_sum = 0; p_n = c.count;
    esp_ping_handle_t h;
    if (esp_ping_new_session(&c, &cb, &h)) return -3;
    esp_ping_start(h);
    int ok = xSemaphoreTake(psem, pdMS_TO_TICKS(c.count * 1100 + 2000));
    esp_ping_stop(h); esp_ping_delete_session(h);
    printf("ping ok=%u of %u avg_ms=%u\n", (unsigned)p_ok, (unsigned)p_n, p_ok ? (unsigned)(p_time_sum / p_ok) : 0);
    return ok ? 0 : -4;
}

static void udp_task(void *a)
{
    int port = (int)a, s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(s, (void *)&sa, sizeof sa);
    static uint8_t buf[1600];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf, 0, (void *)&from, &fl);
        if (n > 0) sendto(s, buf, n, 0, (void *)&from, fl);
    }
}
static void tcp_conn_task(void *a)
{
    int c = (int)a; static uint8_t buf[2048];
    for (int n; (n = recv(c, buf, sizeof buf, 0)) > 0;) send(c, buf, n, 0);
    close(c); vTaskDelete(0);
}
static void tcp_task(void *a)
{
    int port = (int)a, s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(s, (void *)&sa, sizeof sa); listen(s, 2);
    for (;;) { int c = accept(s, 0, 0); if (c >= 0) xTaskCreate(tcp_conn_task, "tcpc", 4096, (void *)c, 5, 0); }
}
static int cmd_udp_echo(int argc, char **argv) { return argc < 2 ? -1 : (xTaskCreate(udp_task, "udp", 4096, (void *)atoi(argv[1]), 5, 0) == pdPASS ? 0 : -2); }
static int cmd_tcp_echo(int argc, char **argv) { return argc < 2 ? -1 : (xTaskCreate(tcp_task, "tcp", 4096, (void *)atoi(argv[1]), 5, 0) == pdPASS ? 0 : -2); }

static int dial(const char *ip, int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_aton(ip, &sa.sin_addr);
    if (connect(s, (void *)&sa, sizeof sa)) { close(s); return -1; }
    return s;
}
static int cmd_tcp_send(int argc, char **argv)                   /* tcp_send <ip> <port> <bytes>: pattern i&0xff, then read the echo back and compare */
{
    if (argc < 4) return -1;
    int s = dial(argv[1], atoi(argv[2])); if (s < 0) return -2;
    unsigned total = atoi(argv[3]), sent = 0, got = 0, bad = 0; uint8_t buf[1024];
    struct timeval tv = { 5, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (got < total) {
        if (sent < total) { unsigned n = total - sent > sizeof buf ? sizeof buf : total - sent; for (unsigned i = 0; i < n; i++) buf[i] = (sent + i) & 0xFF; int w = send(s, buf, n, 0); if (w > 0) sent += w; }
        int r = recv(s, buf, sizeof buf, sent < total ? MSG_DONTWAIT : 0);
        if (r > 0) { for (int i = 0; i < r; i++) if (buf[i] != ((got + i) & 0xFF)) bad++; got += r; }
        else if (r == 0 || (r < 0 && sent >= total)) break;
    }
    close(s);
    printf("tcp_send sent=%u echoed=%u bad=%u\n", sent, got, bad);
    return got == total && !bad ? 0 : -3;
}
static int cmd_iperf_tx(int argc, char **argv)                   /* iperf_tx <ip> <port> <seconds>: sink side must read */
{
    if (argc < 4) return -1;
    int s = dial(argv[1], atoi(argv[2])); if (s < 0) return -2;
    static uint8_t buf[1400]; memset(buf, 0x5A, sizeof buf);
    uint64_t t0 = esp_timer_get_time(), bytes = 0;
    while (esp_timer_get_time() - t0 < (uint64_t)atoi(argv[3]) * 1000000) { int w = send(s, buf, sizeof buf, 0); if (w < 0) break; bytes += w; }
    uint64_t us = esp_timer_get_time() - t0; close(s);
    printf("iperf_tx bytes=%llu kbit_s=%llu\n", bytes, bytes * 8000 / us);
    return 0;
}
static int cmd_iperf_rx(int argc, char **argv)                   /* iperf_rx <port> <seconds>: accept one connection, count bytes */
{
    if (argc < 3) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(atoi(argv[1])), .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(s, (void *)&sa, sizeof sa); listen(s, 1);
    struct timeval tv = { atoi(argv[2]) + 10, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int c = accept(s, 0, 0); close(s);
    if (c < 0) return -2;
    static uint8_t buf[2048]; uint64_t t0 = esp_timer_get_time(), bytes = 0;
    for (int n; (n = recv(c, buf, sizeof buf, 0)) > 0;) bytes += n;
    uint64_t us = esp_timer_get_time() - t0; close(c);
    printf("iperf_rx bytes=%llu kbit_s=%llu\n", bytes, us ? bytes * 8000 / us : 0);
    return 0;
}

static int cmd_http_get(int argc, char **argv)                    /* http_get <url> [n]: n sequential GETs, one connection each */
{
    if (argc < 2) return -1;
    int reps = argc > 2 ? atoi(argv[2]) : 1, good = 0; int64_t t0 = esp_timer_get_time(); unsigned bytes = 0;
    for (int r = 0; r < reps; r++) {
        esp_http_client_config_t c = { .url = argv[1], .timeout_ms = 8000 };
        esp_http_client_handle_t h = esp_http_client_init(&c);
        if (esp_http_client_open(h, 0)) { esp_http_client_cleanup(h); continue; }
        esp_http_client_fetch_headers(h);
        int status = esp_http_client_get_status_code(h); unsigned total = 0; char buf[512];
        for (int n; (n = esp_http_client_read(h, buf, sizeof buf)) > 0;) total += n;
        esp_http_client_cleanup(h);
        if (status == 200) { good++; bytes += total; }
        if (r == reps - 1) printf("http status=%d bytes=%u\n", status, total);
    }
    printf("http ok=%d of %d elapsed_ms=%u bytes=%u\n", good, reps, (unsigned)((esp_timer_get_time() - t0) / 1000), bytes);
    return good == reps ? 0 : -3;
}
/* http_raw <ip> <port> <nocrlf|badline|longline>: a malformed request; prints the first response line, or "closed" */
static int cmd_http_raw(int argc, char **argv)
{
    if (argc < 4) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(atoi(argv[2])) };
    inet_aton(argv[1], &a.sin_addr);
    struct timeval tv = { 5, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -2; }
    static char junk[2400];
    if (!strcmp(argv[3], "nocrlf")) { memset(junk, 'A', 2000); send(fd, junk, 2000, 0); }
    else if (!strcmp(argv[3], "badline")) send(fd, "GARBAGE\r\n\r\n", 12, 0);
    else { memset(junk, 'B', 300); memcpy(junk, "GET /", 5); memcpy(junk + 298, " HTTP/1.1\r\n\r\n", 0); send(fd, junk, 300, 0); send(fd, " HTTP/1.1\r\n\r\n", 13, 0); }
    char buf[96] = { 0 }; int n = recv(fd, buf, sizeof buf - 1, 0);
    char *e = strchr(buf, '\r'); if (e) *e = 0;
    printf("raw %s\n", n > 0 ? buf : "closed");
    close(fd);
    return 0;
}
static esp_err_t hello_h(httpd_req_t *r) { return httpd_resp_sendstr(r, "hello"); }
static esp_err_t big_h(httpd_req_t *r)
{
    static char chunk[1024]; memset(chunk, 'x', sizeof chunk);
    for (int i = 0; i < 64; i++) httpd_resp_send_chunk(r, chunk, sizeof chunk);
    return httpd_resp_send_chunk(r, 0, 0);
}
extern const char ca_pem_start[] asm("_binary_ca_pem_start"), ca_pem_end[] asm("_binary_ca_pem_end"), other_ca_pem_start[] asm("_binary_other_ca_pem_start"), other_ca_pem_end[] asm("_binary_other_ca_pem_end"),
                  server_pem_start[] asm("_binary_server_pem_start"), server_pem_end[] asm("_binary_server_pem_end"), server_key_pem_start[] asm("_binary_server_key_pem_start"), server_key_pem_end[] asm("_binary_server_key_pem_end");

/* https_get <url> [other|-] [n] [host]: n sequential GETs over TLS; trust is our test CA (tools/wl/pki), or with "other" an unrelated one; the certificate name is checked against host (default ndf.local) */
static int cmd_https_get(int argc, char **argv)
{
    if (argc < 2) return -1;
    int other = argc > 2 && !strcmp(argv[2], "other"), reps = argc > 3 ? atoi(argv[3]) : 1, good = 0; const char *host = argc > 4 ? argv[4] : "ndf.local"; int64_t t0 = esp_timer_get_time(); unsigned bytes = 0;
    for (int r = 0; r < reps; r++) {
        esp_http_client_config_t c = { .url = argv[1], .timeout_ms = 10000, .cert_pem = other ? other_ca_pem_start : ca_pem_start, .cert_len = (size_t)((other ? other_ca_pem_end : ca_pem_end) - (other ? other_ca_pem_start : ca_pem_start)), .common_name = host };
        esp_http_client_handle_t h = esp_http_client_init(&c);
        if (esp_http_client_open(h, 0)) { esp_http_client_cleanup(h); continue; }
        esp_http_client_fetch_headers(h);
        int status = esp_http_client_get_status_code(h); unsigned total = 0; char buf[512];
        for (int n; (n = esp_http_client_read(h, buf, sizeof buf)) > 0;) total += n;
        esp_http_client_cleanup(h);
        if (status == 200) { good++; bytes += total; }
        if (r == reps - 1) printf("https status=%d bytes=%u\n", status, total);
    }
    printf("https ok=%d of %d elapsed_ms=%u bytes=%u\n", good, reps, (unsigned)((esp_timer_get_time() - t0) / 1000), bytes);
    return good == reps ? 0 : -3;
}
static esp_err_t hello_h(httpd_req_t *r);
static esp_err_t big_h(httpd_req_t *r);
static int cmd_https_serve(int argc, char **argv)                  /* https_serve [port]: esp_https_server with the test certificate; /hello and /big as http_serve */
{
    httpd_ssl_config_t c = HTTPD_SSL_CONFIG_DEFAULT(); httpd_handle_t h;
    if (argc > 1) c.port_secure = atoi(argv[1]);
    c.servercert = (const uint8_t *)server_pem_start; c.servercert_len = (size_t)(server_pem_end - server_pem_start);
    c.prvtkey_pem = (const uint8_t *)server_key_pem_start; c.prvtkey_len = (size_t)(server_key_pem_end - server_key_pem_start);
    if (httpd_ssl_start(&h, &c)) return -1;
    httpd_uri_t u1 = { .uri = "/hello", .method = HTTP_GET, .handler = hello_h }, u2 = { .uri = "/big", .method = HTTP_GET, .handler = big_h };
    httpd_register_uri_handler(h, &u1); httpd_register_uri_handler(h, &u2);
    return 0;
}
/* tls_prims: the TLS 1.3 building blocks on mbedtls (what esp-tls uses), timed the way components/tls/test_apps/tls_unit times ours */
static int cmd_tls_prims(int argc, char **argv)
{
    mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg; mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&drbg); mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, 0, 0);
    int64_t t; unsigned x_base, x_shared, sign, verify;
    mbedtls_ecdh_context a, b; mbedtls_ecdh_init(&a); mbedtls_ecdh_init(&b);
    mbedtls_ecdh_setup(&a, MBEDTLS_ECP_DP_CURVE25519); mbedtls_ecdh_setup(&b, MBEDTLS_ECP_DP_CURVE25519);
    unsigned char pa[64], pb[64], sec[64]; size_t la = 0, lb = 0, ls = 0;
    t = esp_timer_get_time(); mbedtls_ecdh_make_public(&a, &la, pa, sizeof pa, mbedtls_ctr_drbg_random, &drbg); x_base = (unsigned)(esp_timer_get_time() - t);
    mbedtls_ecdh_make_public(&b, &lb, pb, sizeof pb, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ecdh_read_public(&a, pb, lb);
    t = esp_timer_get_time(); mbedtls_ecdh_calc_secret(&a, &ls, sec, sizeof sec, mbedtls_ctr_drbg_random, &drbg); x_shared = (unsigned)(esp_timer_get_time() - t);
    mbedtls_ecdsa_context e; mbedtls_ecdsa_init(&e); mbedtls_ecdsa_genkey(&e, MBEDTLS_ECP_DP_SECP256R1, mbedtls_ctr_drbg_random, &drbg);
    unsigned char h[32] = { 1 }, sig[80]; size_t sl = 0;
    t = esp_timer_get_time(); mbedtls_ecdsa_write_signature(&e, MBEDTLS_MD_SHA256, h, 32, sig, sizeof sig, &sl, mbedtls_ctr_drbg_random, &drbg); sign = (unsigned)(esp_timer_get_time() - t);
    t = esp_timer_get_time(); int vr = mbedtls_ecdsa_read_signature(&e, h, 32, sig, sl); verify = (unsigned)(esp_timer_get_time() - t);
    mbedtls_gcm_context g; mbedtls_gcm_init(&g); unsigned char key[16] = { 0 }, iv[12] = { 0 }, tag[16]; static unsigned char buf[4096];
    mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
    t = esp_timer_get_time(); for (int i = 0; i < 256; i++) mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, sizeof buf, iv, 12, 0, 0, buf, buf, 16, tag); unsigned gcm_us = (unsigned)(esp_timer_get_time() - t);
    printf("mbedtls x25519_base_us=%u x25519_shared_us=%u ecdsa_sign_us=%u ecdsa_verify_us=%u(%d) gcm_1MB_us=%u (4096-byte calls, tag over 16 B)\n", x_base, x_shared, sign, verify, vr, gcm_us);
    return vr ? -1 : 0;
}
static int cmd_http_serve(int argc, char **argv)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG(); httpd_handle_t h;
    if (argc > 1) c.server_port = atoi(argv[1]);
    if (httpd_start(&h, &c)) return -1;
    httpd_uri_t u1 = { .uri = "/hello", .method = HTTP_GET, .handler = hello_h }, u2 = { .uri = "/big", .method = HTTP_GET, .handler = big_h };
    httpd_register_uri_handler(h, &u1); httpd_register_uri_handler(h, &u2);
    return 0;
}

const peer_cmd_t cmds_net[] = { { "ping", cmd_ping }, { "udp_echo", cmd_udp_echo }, { "tcp_echo", cmd_tcp_echo }, { "tcp_send", cmd_tcp_send },
                                { "iperf_tx", cmd_iperf_tx }, { "iperf_rx", cmd_iperf_rx }, { "http_get", cmd_http_get }, { "http_raw", cmd_http_raw }, { "http_serve", cmd_http_serve }, { "https_get", cmd_https_get }, { "tls_prims", cmd_tls_prims }, { "https_serve", cmd_https_serve }, { 0 } };
