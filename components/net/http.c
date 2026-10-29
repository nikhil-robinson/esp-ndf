#pragma GCC diagnostic ignored "-Wunused-parameter"      /* lwIP callback signatures */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "http.h"
#include "net.h"
#include "sys.h"
#include "lwip/tcp.h"

#ifndef NDF_HTTP_CONNS
#define NDF_HTTP_CONNS  2
#endif
#define MAX_ROUTES 8
#define REQ_MAX    512                                    /* request line + headers; more than this without an empty line is a 400 */
#define IDLE_POLLS 5                                      /* x 2 s */

/* ---------------- server ---------------- */
static struct route { const char *m, *p; int (*h)(const http_req_t *, http_resp_t *); } routes[MAX_ROUTES];
static unsigned n_routes;
int http_route(const char *method, const char *path, int (*h)(const http_req_t *, http_resp_t *))
{
    if (n_routes == MAX_ROUTES) return -1;
    routes[n_routes++] = (struct route){ method, path, h };
    return 0;
}

typedef struct { struct tcp_pcb *pcb; char req[REQ_MAX + 1]; uint16_t rl; uint8_t busy, idle, close_after; char hdr[160]; uint16_t hlen, hoff; const uint8_t *body; uint32_t blen, boff; } conn_t;
static conn_t pool[NDF_HTTP_CONNS];
static struct tcp_pcb *lpcb;

static void conn_free(conn_t *c) { if (c->pcb) { tcp_arg(c->pcb, 0); tcp_recv(c->pcb, 0); tcp_sent(c->pcb, 0); tcp_poll(c->pcb, 0, 0); tcp_err(c->pcb, 0); if (tcp_close(c->pcb)) tcp_abort(c->pcb); } memset(c, 0, sizeof *c); }

static void pump_out(conn_t *c)                                          /* push as much of header + body as the send buffer takes */
{
    while (c->hoff < c->hlen) {
        u16_t room = tcp_sndbuf(c->pcb); unsigned n = c->hlen - c->hoff; if (n > room) n = room;
        if (!n || tcp_write(c->pcb, c->hdr + c->hoff, (u16_t)n, TCP_WRITE_FLAG_COPY)) { tcp_output(c->pcb); return; }
        c->hoff = (uint16_t)(c->hoff + n);
    }
    while (c->boff < c->blen) {
        u16_t room = tcp_sndbuf(c->pcb); uint32_t n = c->blen - c->boff; if (n > room) n = room; if (n > 1460) n = 1460;
        if (!n || tcp_write(c->pcb, c->body + c->boff, (u16_t)n, 0)) break;       /* body is constant data: no copy */
        c->boff += n;
    }
    tcp_output(c->pcb);
    if (c->hoff == c->hlen && c->boff == c->blen && c->busy) {                   /* all queued: the connection is ready for the next request */
        c->busy = 0; c->rl = 0;
        if (c->close_after) conn_free(c);
    }
}

static int lc(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
static int has_close(const char *h)                                       /* a "Connection: close" header line (any case) */
{
    for (; *h; h++) {
        static const char k[] = "connection:";
        unsigned i = 0; while (k[i] && lc(h[i]) == k[i]) i++;
        if (k[i]) continue;
        const char *v = h + i; while (*v == ' ') v++;
        return lc(v[0]) == 'c' && lc(v[1]) == 'l' && lc(v[2]) == 'o' && lc(v[3]) == 's' && lc(v[4]) == 'e';
    }
    return 0;
}

static const char *reason_of(int status) { return status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Error"; }
unsigned http_head(char *out, unsigned max, int status, const char *ctype, unsigned long len, int close_after)
{
    int n = snprintf(out, max, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: %s\r\n\r\n", status, reason_of(status), ctype ? ctype : "text/plain", len, close_after ? "close" : "keep-alive");
    return n < 0 ? 0 : (unsigned)n < max ? (unsigned)n : max - 1;
}

static void respond(conn_t *c, int status, const char *ctype, const uint8_t *body, uint32_t len)
{
    c->hlen = (uint16_t)http_head(c->hdr, sizeof c->hdr, status, ctype, len, c->close_after);
    c->hoff = 0; c->body = body; c->blen = len; c->boff = 0; c->busy = 1;
    pump_out(c);
}

/* req = the request head, NUL-terminated and ending in an empty line (modified). Fills rs; *close_after tells whether the connection ends with the answer. */
void http_dispatch(char *req, http_resp_t *rs, int *close_after)
{
    static const uint8_t bad[] = "bad request\n", nf[] = "not found\n", err[] = "error\n";
    char *eol = strstr(req, "\r\n"); *eol = 0;                           /* the request line ends before the first CRLF (the blank line was found by the caller) */
    char *sp1 = strchr(req, ' '); char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : 0;
    if (!sp1 || !sp2 || sp1 - req > 7 || sp2 - sp1 > 129 || sp1[1] != '/' || strncmp(sp2 + 1, "HTTP/1.", 7)) { *close_after = 1; *rs = (http_resp_t){ 400, "text/plain", bad, sizeof bad - 1 }; return; }
    *sp1 = 0; *sp2 = 0;
    *close_after = sp2[8] == '0' || has_close(eol + 2);
    http_req_t rq = { req, sp1 + 1 };
    for (unsigned i = 0; i < n_routes; i++)
        if (!strcmp(routes[i].m, rq.method) && !strcmp(routes[i].p, rq.path)) {
            *rs = (http_resp_t){ 200, "text/plain", 0, 0 };
            if (routes[i].h(&rq, rs)) { *close_after = 1; *rs = (http_resp_t){ 500, "text/plain", err, sizeof err - 1 }; }
            return;
        }
    *rs = (http_resp_t){ 404, "text/plain", nf, sizeof nf - 1 };
}

static void handle_request(conn_t *c)
{
    c->req[c->rl] = 0;
    http_resp_t rs; int ca; http_dispatch(c->req, &rs, &ca); c->close_after = (uint8_t)ca;
    respond(c, rs.status, rs.ctype, rs.body, rs.len);
}

static err_t s_recv(void *a, struct tcp_pcb *p, struct pbuf *q, err_t e)
{
    conn_t *c = a;
    if (!q) { conn_free(c); return ERR_OK; }                                  /* peer closed */
    c->idle = 0;
    if (c->busy) { tcp_recved(p, q->tot_len); pbuf_free(q); return ERR_OK; }  /* pipelined bytes while answering: ignored */
    for (struct pbuf *b = q; b; b = b->next) {
        unsigned n = b->len; if (c->rl + n > REQ_MAX) n = REQ_MAX - c->rl;
        memcpy(c->req + c->rl, b->payload, n); c->rl = (uint16_t)(c->rl + n);
    }
    tcp_recved(p, q->tot_len); pbuf_free(q);
    c->req[c->rl] = 0;
    if (strstr(c->req, "\r\n\r\n")) handle_request(c);
    else if (c->rl >= REQ_MAX) { c->close_after = 1; respond(c, 400, "text/plain", (const uint8_t *)"header too long\n", 16); }   /* no empty line within the limit */
    return ERR_OK;
}
static err_t s_sent(void *a, struct tcp_pcb *p, u16_t len) { conn_t *c = a; if (c->busy) pump_out(c); return ERR_OK; }
static err_t s_poll(void *a, struct tcp_pcb *p)
{
    conn_t *c = a;
    if (c->busy) { pump_out(c); return ERR_OK; }
    if (++c->idle >= IDLE_POLLS) conn_free(c);                                 /* idle keep-alive or a half-sent request */
    return ERR_OK;
}
static void s_err(void *a, err_t e) { conn_t *c = a; c->pcb = 0; memset(c, 0, sizeof *c); }          /* lwIP already freed the pcb */

static err_t s_accept(void *a, struct tcp_pcb *p, err_t e)
{
    for (unsigned i = 0; i < NDF_HTTP_CONNS; i++)
        if (!pool[i].pcb) {
            conn_t *c = &pool[i]; memset(c, 0, sizeof *c); c->pcb = p;
            tcp_arg(p, c); tcp_recv(p, s_recv); tcp_sent(p, s_sent); tcp_poll(p, s_poll, 4); tcp_err(p, s_err);
            return ERR_OK;
        }
    return ERR_MEM;                                                           /* pool full: lwIP aborts the new connection */
}

int http_server_start(uint16_t port)
{
    struct tcp_pcb *p = tcp_new();
    if (!p || tcp_bind(p, IP_ADDR_ANY, port)) return -1;
    lpcb = tcp_listen(p); if (!lpcb) return -1;
    tcp_accept(lpcb, s_accept);
    return 0;
}
void http_server_stop(void) { for (unsigned i = 0; i < NDF_HTTP_CONNS; i++) if (pool[i].pcb) conn_free(&pool[i]); if (lpcb) { tcp_close(lpcb); lpcb = 0; } }

/* ---------------- client ---------------- */
static struct { struct tcp_pcb *pcb; int state; /* 0 connecting, 1 headers, 2 body, 3 done, -1 error */ char hdr[REQ_MAX + 1]; unsigned hl;
                uint8_t *out; unsigned max, got; int status; long clen; int chunked; long csz; int cst; /* chunk state: 0 size, 1 ext, 2 data, 3 CR, 4 LF, 5 trailer */ unsigned cnt; } g;

static void body_byte(uint8_t b)
{
    if (!g.chunked) { if (g.got < g.max) g.out[g.got] = b; g.got++; if (g.clen >= 0 && (long)g.got >= g.clen) g.state = 3; return; }
    switch (g.cst) {
    case 0: if (b == '\r') g.cst = 4; else if (b == ';') g.cst = 1; else { int d = b >= '0' && b <= '9' ? b - '0' : (b | 32) >= 'a' && (b | 32) <= 'f' ? (b | 32) - 'a' + 10 : -1; if (d < 0 || g.csz > 0xFFFFFF) { g.state = -1; return; } g.csz = g.csz * 16 + d; } break;
    case 1: if (b == '\r') g.cst = 4; break;
    case 4: if (b != '\n') { g.state = -1; return; } if (g.csz == 0) { g.state = 3; return; } g.cst = 2; break;
    case 2: if (g.got < g.max) g.out[g.got] = b; g.got++; if (--g.csz == 0) g.cst = 3; break;
    case 3: if (b != '\r') { g.state = -1; return; } g.cst = 5; break;
    case 5: if (b != '\n') { g.state = -1; return; } g.cst = 0; g.csz = 0; break;
    }
}

int http_resp_feed(const uint8_t *p, unsigned n)                             /* decoded bytes of a response; the client state afterwards */
{
    for (unsigned i = 0; i < n && g.state >= 1 && g.state < 3; i++) {
        uint8_t ch = p[i];
        if (g.state == 1) {
            if (g.hl >= REQ_MAX) { g.state = -1; break; }
            g.hdr[g.hl++] = (char)ch; g.hdr[g.hl] = 0;
            if (g.hl >= 4 && !memcmp(g.hdr + g.hl - 4, "\r\n\r\n", 4)) {
                g.status = strncmp(g.hdr, "HTTP/1.", 7) ? 0 : atoi(g.hdr + 9);
                g.clen = -1; g.chunked = 0;
                for (char *l = strstr(g.hdr, "\r\n"); l && *l; l = strstr(l + 2, "\r\n")) {
                    if (!strncmp(l + 2, "Content-Length:", 15)) g.clen = atol(l + 17);
                    else if (!strncmp(l + 2, "Transfer-Encoding: chunked", 26)) g.chunked = 1;
                }
                if (!g.status) g.state = -1; else if (!g.chunked && g.clen == 0) g.state = 3; else g.state = 2;
            }
        } else body_byte(ch);
    }
    return g.state;
}
void http_resp_begin(uint8_t *out, unsigned max) { memset(&g, 0, sizeof g); g.out = out; g.max = max; g.clen = -1; g.state = 1; }
int http_resp_end(int *status)                                                /* bytes received, -1 on error; the transport closed (or not) */
{
    if (status) *status = g.status;
    return g.state == 3 || (g.state == 2 && g.clen < 0 && !g.chunked) ? (int)g.got : -1;
}

static err_t c_recv(void *a, struct tcp_pcb *p, struct pbuf *q, err_t e)
{
    if (!q) { if (g.state < 3) g.state = g.state == 2 && g.clen < 0 && !g.chunked ? 3 : -1; return ERR_OK; }       /* close ends a body without a length */
    for (struct pbuf *b = q; b && g.state >= 1 && g.state < 3; b = b->next) http_resp_feed(b->payload, b->len);
    tcp_recved(p, q->tot_len); pbuf_free(q);
    return ERR_OK;
}
static err_t c_conn(void *a, struct tcp_pcb *p, err_t e) { if (!e) g.state = 1; else g.state = -1; return ERR_OK; }
static void c_err(void *a, err_t e) { g.state = -1; g.pcb = 0; }

int http_get(const char *url, uint8_t *body, unsigned max, int *status)
{
    if (strncmp(url, "http://", 7)) return -1;
    const char *h = url + 7; ip4_addr_t ip; unsigned b[4]; uint16_t port = 80;
    for (int i = 0; i < 4; i++) { char *e; b[i] = (unsigned)strtoul(h, &e, 10); if (e == h || b[i] > 255) return -1; h = e + (*e == '.'); }
    IP4_ADDR(&ip, b[0], b[1], b[2], b[3]);
    if (*h == ':') { port = (uint16_t)atoi(h + 1); while (*h && *h != '/') h++; }
    const char *path = *h == '/' ? h : "/";
    memset(&g, 0, sizeof g); g.out = body; g.max = max; g.clen = -1;
    g.pcb = tcp_new(); if (!g.pcb) return -1;
    tcp_recv(g.pcb, c_recv); tcp_err(g.pcb, c_err);
    if (tcp_connect(g.pcb, &ip, port, c_conn)) { tcp_abort(g.pcb); return -1; }
    uint32_t t0 = time_ms();
    while (g.state == 0 && time_ms() - t0 < 4000) net_poll();
    if (g.state == 1) {
        char rq[200]; int n = snprintf(rq, sizeof rq, "GET %s HTTP/1.1\r\nHost: %u.%u.%u.%u\r\nConnection: close\r\n\r\n", path, b[0], b[1], b[2], b[3]);
        if (n <= 0 || n >= (int)sizeof rq || tcp_write(g.pcb, rq, (u16_t)n, TCP_WRITE_FLAG_COPY)) g.state = -1; else tcp_output(g.pcb);
    }
    while (g.state >= 1 && g.state < 3 && time_ms() - t0 < 10000) net_poll();
    int ok = g.state == 3 || (g.state == 2 && g.clen < 0 && !g.chunked);
    if (g.pcb) { tcp_arg(g.pcb, 0); tcp_recv(g.pcb, 0); tcp_err(g.pcb, 0); if (tcp_close(g.pcb)) tcp_abort(g.pcb); g.pcb = 0; }
    if (status) *status = g.status;
    return ok ? (int)g.got : -1;
}
