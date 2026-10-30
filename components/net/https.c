#pragma GCC diagnostic ignored "-Wunused-parameter"      /* lwIP callback signatures */
/* HTTPS: the HTTP server (routes of http.c) and a blocking client over the TLS engine. The TLS engine is sans-IO: bytes received on the pcb go into
 * tls_feed(), decrypted data comes out of tls_read(), whatever tls_out() has queued is written to the pcb. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "https.h"
#include "net.h"
#include "sys.h"
#include "lwip/tcp.h"

#define REQ_MAX    512
#define IDLE_POLLS 5                                      /* x 2 s: idle connections are freed after 10 s */
#define POLL_TICKS (2000 / (2 * TCP_TMR_INTERVAL))       /* tcp_poll counts lwIP slow-timer ticks (2 x TCP_TMR_INTERVAL): one call every 2 s whatever the timer */
#ifndef HTTPS_TIMEOUT_MS
#define HTTPS_TIMEOUT_MS 90000                /* whole request: connect, handshake, body (a slow link needs minutes for 64 KB) */
#endif

/* ---------------- shared: move queued TLS bytes to the pcb ---------------- */
static void flush(struct tcp_pcb *pcb, tls_t *t)
{
    unsigned n; const uint8_t *o = tls_out(t, &n);
    while (n) {
        u16_t room = tcp_sndbuf(pcb); unsigned k = n < room ? n : room; if (k > 1460) k = 1460;
        if (!k || tcp_write(pcb, o, (u16_t)k, TCP_WRITE_FLAG_COPY)) break;
        tls_out_done(t, k); o = tls_out(t, &n);
    }
    tcp_output(pcb);
}

/* One tls_t (about 7 KB) serves the server connection and the client: only one TLS connection at a time, the other side is refused while it is in use. */
static tls_t tls_inst; static uint8_t tls_busy;

/* ---------------- server ---------------- */
typedef struct { struct tcp_pcb *pcb; char req[REQ_MAX + 1]; uint16_t rl; uint8_t busy, idle, close_after, closing; char hdr[160]; uint16_t hlen, hoff; const uint8_t *body; uint32_t blen, boff; } sconn_t;
static sconn_t sc;
static struct tcp_pcb *lpcb, *waiting; static const tls_server_cfg_t *scfg;   /* waiting: a connection that arrived while the TLS slot was busy (served next, as a listen backlog) */
static void start(struct tcp_pcb *p);

static void conn_free(sconn_t *c)
{
    if (c->pcb) { tcp_arg(c->pcb, 0); tcp_recv(c->pcb, 0); tcp_sent(c->pcb, 0); tcp_poll(c->pcb, 0, 0); tcp_err(c->pcb, 0); if (tcp_close(c->pcb)) tcp_abort(c->pcb); tls_busy = 0; }
    memset(c, 0, sizeof *c);
    if (waiting && !tls_busy) { struct tcp_pcb *w = waiting; waiting = 0; start(w); }
}

static void finish(sconn_t *c)                                           /* answer sent: next request, or close_notify and close */
{
    c->busy = 0; c->rl = 0;
    if (c->close_after) { tls_close(&tls_inst); c->closing = 1; flush(c->pcb, &tls_inst); }
}

static void pump_out(sconn_t *c)                                         /* header, then body, as far as the TLS output queue takes it */
{
    while (c->hoff < c->hlen) { int w = tls_write(&tls_inst, c->hdr + c->hoff, (unsigned)(c->hlen - c->hoff)); if (w <= 0) goto out; c->hoff = (uint16_t)(c->hoff + w); flush(c->pcb, &tls_inst); }
    while (c->boff < c->blen) { int w = tls_write(&tls_inst, c->body + c->boff, c->blen - c->boff > 4096 ? 4096 : (unsigned)(c->blen - c->boff)); if (w <= 0) goto out; c->boff += (uint32_t)w; flush(c->pcb, &tls_inst); }
out:
    flush(c->pcb, &tls_inst);
    if (c->hoff == c->hlen && c->boff == c->blen && c->busy) finish(c);
}

static void request(sconn_t *c)
{
    c->req[c->rl] = 0;
    http_resp_t rs; int ca; http_dispatch(c->req, &rs, &ca); c->close_after = (uint8_t)ca;
    c->hlen = (uint16_t)http_head(c->hdr, sizeof c->hdr, rs.status, rs.ctype, rs.len, ca);
    c->hoff = 0; c->body = rs.body; c->blen = rs.len; c->boff = 0; c->busy = 1;
    pump_out(c);
}

static void app_bytes(sconn_t *c, const uint8_t *p, unsigned n)
{
    if (c->busy) return;                                                  /* pipelined bytes while answering: ignored */
    if (c->rl + n > REQ_MAX) n = REQ_MAX - c->rl;
    memcpy(c->req + c->rl, p, n); c->rl = (uint16_t)(c->rl + n); c->req[c->rl] = 0;
    if (strstr(c->req, "\r\n\r\n")) request(c);
    else if (c->rl >= REQ_MAX) { c->close_after = 1; http_resp_t rs = { 400, "text/plain", (const uint8_t *)"header too long\n", 16 };   /* no empty line within the limit */
        c->hlen = (uint16_t)http_head(c->hdr, sizeof c->hdr, rs.status, rs.ctype, rs.len, 1); c->hoff = 0; c->body = rs.body; c->blen = rs.len; c->boff = 0; c->busy = 1; pump_out(c); }
}

static err_t s_recv(void *a, struct tcp_pcb *p, struct pbuf *q, err_t e)
{
    sconn_t *c = a;
    if (!q) { conn_free(c); return ERR_OK; }
    c->idle = 0;
    for (struct pbuf *b = q; b && tls_state(&tls_inst) != TLS_FAILED; b = b->next) {
        const uint8_t *d = b->payload; unsigned n = b->len;
        while (n) {
            int u = tls_feed(&tls_inst, d, n); if (u < 0) break;
            d += u; n -= (unsigned)u;
            uint8_t tmp[256]; int r, got = 0;
            while ((r = tls_read(&tls_inst, tmp, sizeof tmp)) > 0) { got = 1; app_bytes(c, tmp, (unsigned)r); }
            if (!u && !got) break;                                        /* nothing consumed and nothing to read: avoid spinning */
        }
    }
    tcp_recved(p, q->tot_len); pbuf_free(q);
    if (tls_state(&tls_inst) == TLS_FAILED || tls_state(&tls_inst) == TLS_CLOSED) c->closing = 1;       /* a failed connection sends its alert first */
    flush(p, &tls_inst);
    unsigned left; tls_out(&tls_inst, &left);
    if (c->closing) conn_free(c);                                           /* tcp_close still sends what is queued (the alert, close_notify) */
    return ERR_OK;
}
static err_t s_sent(void *a, struct tcp_pcb *p, u16_t len)
{
    sconn_t *c = a; flush(p, &tls_inst);
    if (c->busy) pump_out(c);
    unsigned left; tls_out(&tls_inst, &left);
    if (c->closing) conn_free(c);                                           /* tcp_close still sends what is queued (the alert, close_notify) */
    return ERR_OK;
}
static err_t s_poll(void *a, struct tcp_pcb *p)
{
    sconn_t *c = a;
    if (c->busy) { pump_out(c); return ERR_OK; }
    if (++c->idle >= IDLE_POLLS || tls_state(&tls_inst) == TLS_FAILED) conn_free(c);          /* idle keep-alive, a stalled handshake or a failed connection */
    return ERR_OK;
}
static void s_err(void *a, err_t e) { sconn_t *c = a; c->pcb = 0; memset(c, 0, sizeof *c); tls_busy = 0; }

static err_t w_err_dummy(void *a, struct tcp_pcb *p, struct pbuf *q, err_t e) { if (q) { tcp_recved(p, 0); return ERR_MEM; } return ERR_OK; }   /* hold its bytes (lwIP keeps them) until it is served */
static void w_err(void *a, err_t e) { waiting = 0; }
static void start(struct tcp_pcb *p)
{
    memset(&sc, 0, sizeof sc); sc.pcb = p; tls_busy = 1;
    tls_server_init(&tls_inst, scfg);
    tcp_arg(p, &sc); tcp_recv(p, s_recv); tcp_sent(p, s_sent); tcp_poll(p, s_poll, POLL_TICKS); tcp_err(p, s_err);
}
static err_t s_accept(void *a, struct tcp_pcb *p, err_t e)
{
    if (!sc.pcb && !tls_busy) { start(p); return ERR_OK; }
    if (waiting) return ERR_MEM;                                          /* slot and backlog full: lwIP refuses the connection */
    waiting = p; tcp_arg(p, 0); tcp_recv(p, w_err_dummy); tcp_err(p, w_err);
    return ERR_OK;
}

int https_server_start(uint16_t port, const tls_server_cfg_t *cfg)
{
    struct tcp_pcb *p = tcp_new();
    if (!p || tcp_bind(p, IP_ADDR_ANY, port)) return -1;
    scfg = cfg; lpcb = tcp_listen(p); if (!lpcb) return -1;
    tcp_accept(lpcb, s_accept);
    return 0;
}
void https_server_stop(void) { conn_free(&sc); if (lpcb) { tcp_close(lpcb); lpcb = 0; } }

/* ---------------- client ---------------- */
static struct { struct tcp_pcb *pcb; int state; /* 0 connecting, 1 connected, 2 request sent, -1 error */ } cl;

static void cl_pump(void)                                                  /* decrypted data to the response parser, queued TLS bytes to the pcb */
{
    uint8_t tmp[256]; int r;
    while ((r = tls_read(&tls_inst, tmp, sizeof tmp)) > 0) http_resp_feed(tmp, (unsigned)r);
    if (cl.pcb) flush(cl.pcb, &tls_inst);
}
static err_t c_recv(void *a, struct tcp_pcb *p, struct pbuf *q, err_t e)
{
    if (!q) { if (cl.state >= 0) cl.state = -2; return ERR_OK; }           /* closed by the peer */
    for (struct pbuf *b = q; b && tls_state(&tls_inst) != TLS_FAILED; b = b->next) {
        const uint8_t *d = b->payload; unsigned n = b->len;
        while (n) { int u = tls_feed(&tls_inst, d, n); if (u < 0) break; d += u; n -= (unsigned)u; cl_pump(); if (!u) break; }
    }
    tcp_recved(p, q->tot_len); pbuf_free(q);
    cl_pump();
    return ERR_OK;
}
static err_t c_conn(void *a, struct tcp_pcb *p, err_t e) { cl.state = e ? -1 : 1; return ERR_OK; }
static void c_err(void *a, err_t e) { cl.state = -1; cl.pcb = 0; }

int https_tls_version(void) { return tls_version(&tls_inst); }
int https_tls_error(void) { return tls_inst.err; }
int https_get(const char *url, const tls_client_cfg_t *cfg, uint8_t *body, unsigned max, int *status)
{
    if (strncmp(url, "https://", 8)) return -1;
    const char *h = url + 8; ip4_addr_t ip; unsigned b[4]; uint16_t port = 443;
    for (int i = 0; i < 4; i++) { char *e; b[i] = (unsigned)strtoul(h, &e, 10); if (e == h || b[i] > 255) return -1; h = e + (*e == '.'); }
    IP4_ADDR(&ip, b[0], b[1], b[2], b[3]);
    if (*h == ':') { port = (uint16_t)atoi(h + 1); while (*h && *h != '/') h++; }
    const char *path = *h == '/' ? h : "/";
    if (tls_busy) return -1;
    memset(&cl, 0, sizeof cl); http_resp_begin(body, max); tls_busy = 1;
    cl.pcb = tcp_new(); if (!cl.pcb) { tls_busy = 0; return -1; }
    tcp_recv(cl.pcb, c_recv); tcp_err(cl.pcb, c_err);
    if (tcp_connect(cl.pcb, &ip, port, c_conn)) { tcp_abort(cl.pcb); tls_busy = 0; return -1; }
    uint32_t t0 = time_ms();
    while (cl.state == 0 && time_ms() - t0 < 4000) net_poll();
    if (cl.state == 1) { tls_client_init(&tls_inst, cfg); cl_pump(); }
    int sent = 0, st = 0;
    while (cl.state >= 1 && time_ms() - t0 < HTTPS_TIMEOUT_MS) {
        net_poll();
        if (tls_state(&tls_inst) == TLS_FAILED) break;
        if (!sent && tls_state(&tls_inst) == TLS_CONNECTED) {
            char rq[240]; int n = snprintf(rq, sizeof rq, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, cfg->host ? cfg->host : "esp-ndf");
            if (n > 0 && n < (int)sizeof rq && tls_write(&tls_inst, rq, (unsigned)n) == n) { sent = 1; cl_pump(); } else break;
        }
        st = http_resp_feed(0, 0);
        if (st == 3 || st < 0) break;
        if (tls_state(&tls_inst) == TLS_CLOSED) { cl_pump(); break; }
    }
    if (cl.pcb) { tls_close(&tls_inst); flush(cl.pcb, &tls_inst); tcp_arg(cl.pcb, 0); tcp_recv(cl.pcb, 0); tcp_err(cl.pcb, 0); if (tcp_close(cl.pcb)) tcp_abort(cl.pcb); cl.pcb = 0; }
    tls_busy = 0;
    return http_resp_end(status);
}
