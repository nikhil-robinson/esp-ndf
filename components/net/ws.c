#pragma GCC diagnostic ignored "-Wunused-parameter"      /* lwIP callback signatures */
/* WebSocket server and client (RFC 6455) on lwIP raw TCP; see ws.h. The server receives connections from http.c's upgrade hook. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "ws.h"
#include "http.h"
#include "net.h"
#include "sys.h"
#include "sha.h"
#include "rng.h"
#include "lwip/tcp.h"

/* ---------------- connections ---------------- */
typedef struct {
    struct tcp_pcb *pcb; uint8_t used, client, closing, close_sent, mop; uint8_t idle;
    uint8_t hdr[14]; uint8_t hl; ws_hdr_t h; uint64_t got;                 /* the frame being received: header bytes, then payload */
    uint8_t ctl[125];                                                       /* a control frame's payload */
    uint8_t msg[NDF_WS_MSG_MAX]; uint32_t ml;                               /* the message being joined */
    ws_msg_cb cb;
} wsc_t;
static wsc_t pool[NDF_WS_CONNS + 1];                                        /* [NDF_WS_CONNS] is the client */
static struct { const char *path; ws_msg_cb cb; } route;
#define CL (&pool[NDF_WS_CONNS])
static int id_of(wsc_t *c) { return c == CL ? WS_CLIENT : (int)(c - pool); }
static wsc_t *conn_of(int id) { wsc_t *c = id == WS_CLIENT ? CL : id >= 0 && id < NDF_WS_CONNS ? &pool[id] : 0; return c && c->used && c->pcb ? c : 0; }

static void drop(wsc_t *c)
{
    ws_msg_cb cb = c->cb; int id = id_of(c);
    if (c->pcb) { tcp_arg(c->pcb, 0); tcp_recv(c->pcb, 0); tcp_poll(c->pcb, 0, 0); tcp_err(c->pcb, 0); if (tcp_close(c->pcb)) tcp_abort(c->pcb); }
    int was = c->used; memset(c, 0, sizeof *c);
    if (was && cb) cb(id, WS_CLOSE, 0, 0);
}
static int send_frame(wsc_t *c, int op, const uint8_t *p, unsigned n)
{
    uint8_t h[14], mask[4]; unsigned hl;
    if (c->client) { rng_fill(mask, 4); hl = ws_frame_head(h, 1, op, n, mask); } else hl = ws_frame_head(h, 1, op, n, 0);
    if (tcp_sndbuf(c->pcb) < hl + n) return -1;
    if (tcp_write(c->pcb, h, (u16_t)hl, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE)) return -1;
    if (c->client) {                                                        /* the client masks: a masked copy, in pieces */
        uint8_t b[256];
        for (unsigned off = 0; off < n; off += sizeof b) { unsigned k = n - off < sizeof b ? n - off : sizeof b; memcpy(b, p + off, k); ws_mask(b, k, mask, off); if (tcp_write(c->pcb, b, (u16_t)k, TCP_WRITE_FLAG_COPY)) return -1; }
    } else if (n && tcp_write(c->pcb, p, (u16_t)n, TCP_WRITE_FLAG_COPY)) return -1;
    tcp_output(c->pcb);
    return 0;
}
static void close_with(wsc_t *c, uint16_t code)                            /* our close frame; we wait for the peer's (or the poll timeout) */
{
    if (!c->close_sent) { uint8_t b[2] = { (uint8_t)(code >> 8), (uint8_t)code }; send_frame(c, WS_CLOSE, b, 2); c->close_sent = 1; }
    c->closing = 1;
}

static void frame_done(wsc_t *c)
{
    ws_hdr_t *h = &c->h;
    if (h->op >= 8) {
        if (h->op == WS_PING) send_frame(c, WS_PONG, c->ctl, (unsigned)h->len);
        else if (h->op == WS_CLOSE) {
            if (h->len == 1) { close_with(c, 1002); return; }
            if (!c->close_sent) { send_frame(c, WS_CLOSE, c->ctl, h->len >= 2 ? 2 : 0); c->close_sent = 1; }   /* echo the status code */
            drop(c);
        }
        return;
    }
    if (h->fin) { if (c->cb) c->cb(id_of(c), c->mop, c->msg, c->ml); c->ml = 0; c->mop = 0; }
}

static void rx_bytes(wsc_t *c, const uint8_t *p, unsigned n)
{
    while (n && c->used) {
        if (c->hl < 14) {                                                   /* header still incomplete (14 = done, payload follows) */
            c->hdr[c->hl++] = *p++; n--;
            int r = ws_frame_parse(c->hdr, c->hl, &c->h);
            if (r < 0) { close_with(c, 1002); return; }
            if (r == 0) continue;
            ws_hdr_t *h = &c->h;
            if (h->masked != !c->client) { close_with(c, 1002); return; }   /* client frames are masked, server frames are not */
            if (h->op < 8) {                                                /* data: start or continue a message */
                if (h->op == WS_CONT ? !c->mop : c->mop) { close_with(c, 1002); return; }
                if (h->op) c->mop = h->op;
                if (c->ml + h->len > NDF_WS_MSG_MAX) { close_with(c, 1009); return; }   /* message too big */
            }
            c->got = 0; c->hl = 14;                                         /* 14: header done, payload follows */
            if (!h->len) { frame_done(c); c->hl = 0; }
            continue;
        }
        ws_hdr_t *h = &c->h; unsigned k = (unsigned)(h->len - c->got < n ? h->len - c->got : n);
        uint8_t *dst = h->op >= 8 ? c->ctl + c->got : c->msg + c->ml;
        memcpy(dst, p, k); if (h->masked) ws_mask(dst, k, h->mask, (unsigned)c->got);
        c->got += k; p += k; n -= k; if (h->op < 8) c->ml += k;
        if (c->got == h->len) { c->hl = 0; frame_done(c); }
    }
}

static err_t w_recv(void *a, struct tcp_pcb *p, struct pbuf *q, err_t e)
{
    wsc_t *c = a;
    if (!q) { drop(c); return ERR_OK; }
    c->idle = 0;
    for (struct pbuf *b = q; b && c->used; b = b->next) rx_bytes(c, b->payload, b->len);
    tcp_recved(p, q->tot_len); pbuf_free(q);
    return ERR_OK;
}
static err_t w_poll(void *a, struct tcp_pcb *p) { wsc_t *c = a; if (c->closing && ++c->idle >= 2) drop(c); return ERR_OK; }   /* a close the peer does not answer */
static void w_err(void *a, err_t e) { wsc_t *c = a; c->pcb = 0; drop(c); }
static void attach(wsc_t *c, struct tcp_pcb *p) { c->pcb = p; c->used = 1; tcp_arg(p, c); tcp_recv(p, w_recv); tcp_sent(p, 0); tcp_poll(p, w_poll, 2000 / (2 * TCP_TMR_INTERVAL)); tcp_err(p, w_err); }

/* header value (case-insensitive name) in a request / response head; copies at most max - 1 bytes */
static int header(const char *head, const char *name, char *out, unsigned max)
{
    unsigned nl = (unsigned)strlen(name);
    for (const char *l = strstr(head, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        const char *s = l + 2; unsigned i = 0;
        while (i < nl && s[i] && ((s[i] | 32) == (name[i] | 32))) i++;
        if (i < nl || s[i] != ':') continue;
        s += i + 1; while (*s == ' ') s++;
        unsigned k = 0; while (s[k] && s[k] != '\r' && k + 1 < max) { out[k] = s[k]; k++; }
        out[k] = 0; return 1;
    }
    return 0;
}
static int has_token(const char *v, const char *tok) { for (unsigned tl = (unsigned)strlen(tok); *v; v++) { unsigned i = 0; while (i < tl && (v[i] | 32) == tok[i]) i++; if (i == tl) return 1; } return 0; }

/* http.c hands over a connection whose request head is complete: 1 = taken (101 sent or refused with 400), 0 = not a WebSocket request */
static int upgrade(struct tcp_pcb *p, char *req)
{
    char path[130], key[64], v[64];
    if (strncmp(req, "GET ", 4)) return 0;
    const char *sp = strchr(req + 4, ' '); if (!sp || sp - req - 4 >= (long)sizeof path) return 0;
    memcpy(path, req + 4, (size_t)(sp - req - 4)); path[sp - req - 4] = 0;
    if (!route.path || strcmp(path, route.path) || !header(req, "upgrade", v, sizeof v) || !has_token(v, "websocket")) return 0;
    wsc_t *c = 0; for (unsigned i = 0; i < NDF_WS_CONNS; i++) if (!pool[i].used) { c = &pool[i]; break; }
    const char *bad = 0;
    if (!header(req, "sec-websocket-key", key, sizeof key) || strlen(key) != 24 || !header(req, "sec-websocket-version", v, sizeof v) || strcmp(v, "13")) bad = "HTTP/1.1 400 Bad Request\r\nSec-WebSocket-Version: 13\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    else if (!c) bad = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    if (bad) { tcp_write(p, bad, (u16_t)strlen(bad), TCP_WRITE_FLAG_COPY); tcp_output(p); tcp_arg(p, 0); tcp_recv(p, 0); tcp_sent(p, 0); tcp_poll(p, 0, 0); tcp_err(p, 0); if (tcp_close(p)) tcp_abort(p); return 1; }
    char acc[29], rs[160]; ws_accept_key(key, acc);
    int n = snprintf(rs, sizeof rs, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    memset(c, 0, sizeof *c); c->cb = route.cb; attach(c, p);
    tcp_write(p, rs, (u16_t)n, TCP_WRITE_FLAG_COPY); tcp_output(p);
    return 1;
}

int ws_route(const char *path, ws_msg_cb cb) { route.path = path; route.cb = cb; http_upgrade_hook = upgrade; return 0; }

/* ---------------- client ---------------- */
static struct { int state; char head[512]; unsigned hl; char key[25]; } cs;   /* 0 connecting, 1 waiting for 101, 2 open, -1 failed */
static err_t c_recv(void *a, struct tcp_pcb *p, struct pbuf *q, err_t e)
{
    if (!q) { cs.state = -1; drop(CL); return ERR_OK; }
    unsigned off = 0;
    for (struct pbuf *b = q; b && cs.state == 1; b = b->next)
        for (unsigned i = 0; i < b->len && cs.state == 1; i++) {
            if (cs.hl + 1 >= sizeof cs.head) { cs.state = -1; break; }
            cs.head[cs.hl++] = ((char *)b->payload)[i]; cs.head[cs.hl] = 0; off++;
            if (cs.hl >= 4 && !memcmp(cs.head + cs.hl - 4, "\r\n\r\n", 4)) {
                char v[64], want[29]; ws_accept_key(cs.key, want);
                cs.state = !strncmp(cs.head, "HTTP/1.1 101", 12) && header(cs.head, "sec-websocket-accept", v, sizeof v) && !strcmp(v, want)
                           && header(cs.head, "upgrade", v, sizeof v) && has_token(v, "websocket") ? 2 : -1;
            }
        }
    if (cs.state == 2) {                                                    /* frames right behind the 101 head */
        CL->client = 1; CL->used = 1; tcp_recv(p, w_recv); tcp_arg(p, CL); tcp_poll(p, w_poll, 2000 / (2 * TCP_TMR_INTERVAL));
        unsigned skip = off;
        for (struct pbuf *b = q; b; b = b->next) { if (skip >= b->len) { skip -= b->len; continue; } rx_bytes(CL, (uint8_t *)b->payload + skip, b->len - skip); skip = 0; }
    }
    tcp_recved(p, q->tot_len); pbuf_free(q);
    return ERR_OK;
}
static err_t c_conn(void *a, struct tcp_pcb *p, err_t e) { cs.state = e ? -1 : 1; return ERR_OK; }
static void c_err(void *a, err_t e) { CL->pcb = 0; cs.state = -1; drop(CL); }

int ws_connect(const char *url, ws_msg_cb cb, uint32_t timeout_ms)
{
    if (strncmp(url, "ws://", 5) || CL->used) return -1;
    const char *h = url + 5; ip4_addr_t ip; unsigned b[4]; uint16_t port = 80;
    for (int i = 0; i < 4; i++) { char *e; b[i] = (unsigned)strtoul(h, &e, 10); if (e == h || b[i] > 255) return -1; h = e + (*e == '.'); }
    IP4_ADDR(&ip, b[0], b[1], b[2], b[3]);
    if (*h == ':') { port = (uint16_t)atoi(h + 1); while (*h && *h != '/') h++; }
    const char *path = *h == '/' ? h : "/";
    memset(&cs, 0, sizeof cs); memset(CL, 0, sizeof *CL); CL->cb = cb;
    { static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; uint8_t r[16]; rng_fill(r, 16);   /* 16 random bytes, base64 */
      for (int i = 0, o = 0; i < 18; i += 3) { uint32_t v = (uint32_t)r[i] << 16 | (i + 1 < 16 ? (uint32_t)r[i + 1] << 8 : 0) | (i + 2 < 16 ? r[i + 2] : 0); cs.key[o++] = b64[v >> 18 & 63]; cs.key[o++] = b64[v >> 12 & 63]; cs.key[o++] = b64[v >> 6 & 63]; cs.key[o++] = b64[v & 63]; }
      cs.key[22] = cs.key[23] = '='; cs.key[24] = 0; }
    struct tcp_pcb *p = tcp_new(); if (!p) return -1;
    CL->pcb = p; tcp_arg(p, CL); tcp_recv(p, c_recv); tcp_err(p, c_err);
    if (tcp_connect(p, &ip, port, c_conn)) { tcp_abort(p); CL->pcb = 0; return -1; }
    uint32_t t0 = time_ms();
    while (cs.state == 0 && time_ms() - t0 < timeout_ms) net_poll();
    if (cs.state == 1) {
        char rq[300]; int n = snprintf(rq, sizeof rq, "GET %s HTTP/1.1\r\nHost: %u.%u.%u.%u:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", path, b[0], b[1], b[2], b[3], port, cs.key);
        if (n <= 0 || n >= (int)sizeof rq || tcp_write(p, rq, (u16_t)n, TCP_WRITE_FLAG_COPY)) cs.state = -1; else tcp_output(p);
    }
    while (cs.state == 1 && time_ms() - t0 < timeout_ms) net_poll();
    if (cs.state != 2) { if (CL->pcb) { CL->used = 1; drop(CL); } memset(CL, 0, sizeof *CL); return -1; }
    return 0;
}

int ws_send(int id, int op, const void *p, unsigned n)
{
    wsc_t *c = conn_of(id);
    if (!c || c->closing || (op != WS_TEXT && op != WS_BIN && op != WS_PING) || (op == WS_PING && n > 125)) return -1;
    return send_frame(c, op, p, n);
}
int ws_close(int id, uint16_t code) { wsc_t *c = conn_of(id); if (!c) return -1; close_with(c, code); return 0; }
int ws_open(int id) { wsc_t *c = conn_of(id); return c && !c->closing; }
