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
#include <sys/time.h>
#include "lwip/netdb.h"
#include "esp_sntp.h"
#include "esp_websocket_client.h"

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
    int64_t t_start = esp_timer_get_time();                          /* after connect, as esp-ndf's ip_unit measures */
    unsigned total = atoi(argv[3]), sent = 0, got = 0, bad = 0; uint8_t buf[1024];
    struct timeval tv = { 5, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (got < total) {
        if (sent < total) { unsigned n = total - sent > sizeof buf ? sizeof buf : total - sent; for (unsigned i = 0; i < n; i++) buf[i] = (sent + i) & 0xFF; int w = send(s, buf, n, 0); if (w > 0) sent += w; }
        int r = recv(s, buf, sizeof buf, sent < total ? MSG_DONTWAIT : 0);
        if (r > 0) { for (int i = 0; i < r; i++) if (buf[i] != ((got + i) & 0xFF)) bad++; got += r; }
        else if (r == 0 || (r < 0 && sent >= total)) break;
    }
    close(s);
    printf("tcp_send sent=%u echoed=%u bad=%u elapsed_ms=%u\n", sent, got, bad, (unsigned)((esp_timer_get_time() - t_start) / 1000));
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
    int other = argc > 2 && !strcmp(argv[2], "other"), reps = argc > 3 ? atoi(argv[3]) : 1, good = 0; const char *host = argc > 4 ? argv[4] : "ndf.local"; int v12 = argc > 5 && !strcmp(argv[5], "12");   /* https_get <url> [other|-] [n] [host] [12] */ int64_t t0 = esp_timer_get_time(); unsigned bytes = 0;
    for (int r = 0; r < reps; r++) {
        esp_http_client_config_t c = { .url = argv[1], .timeout_ms = 10000, .cert_pem = other ? other_ca_pem_start : ca_pem_start, .cert_len = (size_t)((other ? other_ca_pem_end : ca_pem_end) - (other ? other_ca_pem_start : ca_pem_start)), .common_name = host, .tls_version = v12 ? ESP_HTTP_CLIENT_TLS_VER_TLS_1_2 : ESP_HTTP_CLIENT_TLS_VER_ANY };
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


/* --- DNS and SNTP for the esp-ndf client tests (the IDF AP's DHCP hands out 192.168.4.1 as DNS server) and the IDF client twins --- */
static char dns_name[64]; static uint32_t dns_ip; static volatile int dns_mode, sntp_mode;   /* mode 0 normal; dns 1 wrong id, 2 NXDOMAIN; sntp 1 wrong origin, 2 stratum 0 (kiss of death) */
static void dns_task(void *a)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(53) }; bind(s, (struct sockaddr *)&me, sizeof me);
    for (;;) {
        uint8_t q[512]; struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(s, q, sizeof q, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 + 5) continue;
        char name[64]; int o = 12, k = 0;                                           /* QNAME as a dotted string */
        while (o < n && q[o] && k < 62) { int l = q[o++]; if (k) name[k++] = '.'; while (l-- && o < n && k < 62) name[k++] = (char)q[o++]; }
        name[k] = 0; o += 1 + 4;                                                    /* zero label, QTYPE, QCLASS */
        if (o > n) continue;
        int hit = !strcmp(name, dns_name) && dns_mode != 2;
        q[2] = 0x81; q[3] = hit ? 0x80 : 0x83;                                     /* response, recursion; NXDOMAIN when not ours */
        q[6] = 0; q[7] = hit ? 1 : 0; q[8] = q[9] = q[10] = q[11] = 0;
        if (dns_mode == 1) q[1] ^= 0x55;                                           /* wrong transaction id */
        int r = o;
        if (hit) { uint8_t an[16] = { 0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 }; memcpy(an + 12, &dns_ip, 4); memcpy(q + r, an, 16); r += 16; }
        sendto(s, q, r, 0, (struct sockaddr *)&from, fl);
    }
}
static int cmd_dns_serve(int argc, char **argv)                       /* dns_serve <name> <ip> [mode]: A record for one name on UDP 53 */
{
    static int up; if (argc < 3) return -1;
    strlcpy(dns_name, argv[1], sizeof dns_name); dns_ip = inet_addr(argv[2]); dns_mode = argc > 3 ? atoi(argv[3]) : 0;
    if (!up) up = xTaskCreate(dns_task, "dns", 4096, 0, 5, 0) == pdPASS;
    return up ? 0 : -2;
}
static void sntp_task(void *a)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(123) }; bind(s, (struct sockaddr *)&me, sizeof me);
    for (;;) {
        uint8_t q[48]; struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(s, q, sizeof q, 0, (struct sockaddr *)&from, &fl);
        struct timeval tv; gettimeofday(&tv, 0);
        if (n < 48 || (q[0] & 7) != 3) continue;
        uint8_t r[48] = { 0x24, sntp_mode == 2 ? 0 : 2, q[2], 0xec };                 /* LI 0, VN 4, mode 4 server; stratum 2 */
        memcpy(r + 24, q + 40, 8);                                                  /* originate = the client's transmit */
        if (sntp_mode == 1) r[31] ^= 1;
        uint32_t sec = (uint32_t)tv.tv_sec + 2208988800u, fr = (uint32_t)(((uint64_t)tv.tv_usec << 32) / 1000000);
        for (int i = 0; i < 2; i++) { uint8_t *t = r + 32 + 8 * i; t[0] = sec >> 24; t[1] = sec >> 16; t[2] = sec >> 8; t[3] = sec; t[4] = fr >> 24; t[5] = fr >> 16; t[6] = fr >> 8; t[7] = fr; }   /* receive, transmit */
        sendto(s, r, 48, 0, (struct sockaddr *)&from, fl);
    }
}
static int cmd_sntp_serve(int argc, char **argv)                      /* sntp_serve <unix seconds> [mode]: set our clock, answer SNTP on UDP 123 */
{
    static int up; if (argc < 2) return -1;
    struct timeval tv = { .tv_sec = atoll(argv[1]) }; settimeofday(&tv, 0); sntp_mode = argc > 2 ? atoi(argv[2]) : 0;
    if (!up) up = xTaskCreate(sntp_task, "sntps", 4096, 0, 5, 0) == pdPASS;
    return up ? 0 : -2;
}
static int cmd_dns_get(int argc, char **argv)                         /* dns_get <name>: getaddrinfo (lwIP DNS), time in us */
{
    if (argc < 2) return -1;
    int64_t t0 = esp_timer_get_time(); struct addrinfo h = { .ai_family = AF_INET }, *r = 0;
    int e = getaddrinfo(argv[1], 0, &h, &r); int64_t t1 = esp_timer_get_time();
    if (e || !r) { printf("dns_get %s fail %d us %lld\n", argv[1], e, t1 - t0); return 0; }
    printf("dns_get %s ip %s us %lld\n", argv[1], inet_ntoa(((struct sockaddr_in *)r->ai_addr)->sin_addr), t1 - t0); freeaddrinfo(r); return 0;
}
static volatile int sntp_done;
static void sntp_cb(struct timeval *tv) { sntp_done = 1; }
static int cmd_sntp_get(int argc, char **argv)                        /* sntp_get <server ip>: esp_sntp poll mode, time to the first sync in us, then the clock */
{
    if (argc < 2) return -1;
    struct timeval z = { 0 }; settimeofday(&z, 0); sntp_done = 0;
    esp_sntp_stop(); esp_sntp_setoperatingmode(SNTP_OPMODE_POLL); esp_sntp_setservername(0, argv[1]); sntp_set_time_sync_notification_cb(sntp_cb);
    int64_t t0 = esp_timer_get_time(); esp_sntp_init();
    while (!sntp_done && esp_timer_get_time() - t0 < 20000000) vTaskDelay(1);
    int64_t t1 = esp_timer_get_time(); struct timeval tv; gettimeofday(&tv, 0); esp_sntp_stop();
    printf("sntp_get %s us %lld time %lld.%06ld\n", sntp_done ? "ok" : "timeout", t1 - t0, (long long)tv.tv_sec, (long)tv.tv_usec);
    return 0;
}

/* --- WebSocket: esp_http_server echo on :8080/ws, esp_websocket_client against the esp-ndf server --- */
static esp_err_t ws_echo(httpd_req_t *req)
{
    if (req->method == HTTP_GET) return ESP_OK;                                   /* the handshake */
    httpd_ws_frame_t f = { 0 };
    if (httpd_ws_recv_frame(req, &f, 0)) return ESP_FAIL;
    uint8_t *b = f.len ? malloc(f.len) : 0; if (f.len && !b) return ESP_ERR_NO_MEM;
    f.payload = b; esp_err_t e = f.len ? httpd_ws_recv_frame(req, &f, f.len) : ESP_OK;
    if (!e) e = httpd_ws_send_frame(req, &f);
    free(b); return e;
}
static int cmd_ws_serve(int argc, char **argv)                                    /* ws_serve: echo server on :8080/ws */
{
    static httpd_handle_t h; if (h) return 0;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG(); c.server_port = 8080; c.ctrl_port = 32769;
    if (httpd_start(&h, &c)) return -2;
    static const httpd_uri_t u = { .uri = "/ws", .method = HTTP_GET, .handler = ws_echo, .is_websocket = true };
    return httpd_register_uri_handler(h, &u) ? -3 : 0;
}
static volatile int wsc_got, wsc_len, wsc_conn; static int64_t wsc_t;
static void wsc_ev(void *a, esp_event_base_t b, int32_t id, void *d)
{
    esp_websocket_event_data_t *e = d;
    if (id == WEBSOCKET_EVENT_CONNECTED) wsc_conn = 1;
    else if (id == WEBSOCKET_EVENT_DATA && (e->op_code == 1 || e->op_code == 2) && e->payload_offset + e->data_len == e->payload_len) { wsc_len = e->payload_len; wsc_got++; wsc_t = esp_timer_get_time(); }
    else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED) wsc_conn = 0;
}
static int cmd_ws_get(int argc, char **argv)                                      /* ws_get <url> <n> <bytes>: connect, n binary echoes of <bytes>, then a text one; times */
{
    if (argc < 4) return -1;
    int n = atoi(argv[2]), sz = atoi(argv[3]); wsc_got = 0; wsc_conn = 0;
    esp_websocket_client_config_t c = { .uri = argv[1], .buffer_size = 4200, .network_timeout_ms = 5000 };
    int64_t t0 = esp_timer_get_time();
    esp_websocket_client_handle_t h = esp_websocket_client_init(&c);
    esp_websocket_register_events(h, WEBSOCKET_EVENT_ANY, wsc_ev, 0);
    esp_websocket_client_start(h);
    while (!wsc_conn && esp_timer_get_time() - t0 < 5000000) vTaskDelay(1);
    int64_t tc = esp_timer_get_time(); int ok = 0;
    uint8_t *b = malloc(sz ? sz : 1); for (int i = 0; i < sz; i++) b[i] = (uint8_t)i;
    for (int i = 0; i < n && wsc_conn; i++) {
        int want = wsc_got + 1; esp_websocket_client_send_bin(h, (char *)b, sz, pdMS_TO_TICKS(5000));
        int64_t w0 = esp_timer_get_time(); while (wsc_got < want && esp_timer_get_time() - w0 < 5000000) vTaskDelay(1);
        if (wsc_got >= want && wsc_len == sz) ok++;
    }
    int64_t td = esp_timer_get_time();
    int want = wsc_got + 1; esp_websocket_client_send_text(h, "hello", 5, pdMS_TO_TICKS(5000));
    int64_t w0 = esp_timer_get_time(); while (wsc_got < want && esp_timer_get_time() - w0 < 5000000) vTaskDelay(1);
    int text = wsc_got >= want && wsc_len == 5;
    esp_websocket_client_close(h, pdMS_TO_TICKS(2000)); esp_websocket_client_destroy(h); free(b);
    printf("ws_get conn %d connect_ms %d echoes %d of %d text %d echo_ms %d\n", tc - t0 < 5000000, (int)((tc - t0) / 1000), ok, n, text, (int)((td - tc) / 1000));
    return 0;
}
const peer_cmd_t cmds_net[] = { { "ws_serve", cmd_ws_serve }, { "ws_get", cmd_ws_get }, { "dns_serve", cmd_dns_serve }, { "sntp_serve", cmd_sntp_serve }, { "dns_get", cmd_dns_get }, { "sntp_get", cmd_sntp_get }, { "ping", cmd_ping }, { "udp_echo", cmd_udp_echo }, { "tcp_echo", cmd_tcp_echo }, { "tcp_send", cmd_tcp_send },
                                { "iperf_tx", cmd_iperf_tx }, { "iperf_rx", cmd_iperf_rx }, { "http_get", cmd_http_get }, { "http_raw", cmd_http_raw }, { "http_serve", cmd_http_serve }, { "https_get", cmd_https_get }, { "tls_prims", cmd_tls_prims }, { "https_serve", cmd_https_serve }, { 0 } };
