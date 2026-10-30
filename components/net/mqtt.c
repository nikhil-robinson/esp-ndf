#pragma GCC diagnostic ignored "-Wunused-parameter"      /* lwIP callback signatures */
/* MQTT 3.1.1 client on lwIP raw TCP; see mqtt.h. One connection. Timers (keep-alive, QoS 1 retransmit) run from the pcb's tcp_poll. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "mqtt.h"
#include "net.h"
#include "sys.h"
#include "rng.h"
#include "lwip/tcp.h"

static struct {
    struct tcp_pcb *pcb; int state;                 /* 0 idle, 1 TCP connecting, 2 CONNECT sent, 3 connected, -1 failed */
    int connack; uint16_t keepalive, pid; uint32_t last_tx, last_rx;
    uint8_t rx[NDF_MQTT_BUF]; unsigned rl;          /* the packet being received */
    uint8_t out[NDF_MQTT_BUF]; unsigned ol; uint16_t out_pid; uint32_t out_t;   /* our QoS 1 message in flight (for the retransmit) */
    uint16_t wait_pid; int wait_res;               /* SUBACK / UNSUBACK we wait for: result */
    mqtt_msg_cb cb;
} m;

static uint16_t next_pid(void) { if (!++m.pid) m.pid = 1; return m.pid; }
static int raw_send(const uint8_t *p, unsigned n)
{
    if (!m.pcb || tcp_sndbuf(m.pcb) < n || tcp_write(m.pcb, p, (u16_t)n, TCP_WRITE_FLAG_COPY)) return -1;
    tcp_output(m.pcb); m.last_tx = time_ms(); return 0;
}
static void shut(int abort_)
{
    if (m.pcb) { tcp_arg(m.pcb, 0); tcp_recv(m.pcb, 0); tcp_poll(m.pcb, 0, 0); tcp_err(m.pcb, 0); if (abort_ || tcp_close(m.pcb)) tcp_abort(m.pcb); m.pcb = 0; }
    m.state = m.state == 3 || m.state == 2 ? 0 : m.state; m.ol = 0; m.rl = 0;
}

static void packet(const uint8_t *p, unsigned hl, uint32_t rem)
{
    uint8_t tf = p[0]; const uint8_t *v = p + hl;
    switch (tf >> 4) {
    case 2:                                                         /* CONNACK */
        if (m.state != 2 || tf != 0x20 || rem != 2) { m.state = -1; return; }
        m.connack = v[1]; m.state = v[1] ? -1 : 3; return;
    case 3: {                                                       /* PUBLISH */
        const uint8_t *t, *pay; unsigned tl, pl; uint16_t pid;
        if (m.state != 3 || mqtt_parse_publish(v, rem, tf & 15, &t, &tl, &pid, &pay, &pl)) { shut(1); m.state = -1; return; }
        int qos = (tf >> 1) & 3;
        if (qos) { uint8_t a[4] = { qos == 2 ? 0x50 : 0x40, 2, (uint8_t)(pid >> 8), (uint8_t)pid }; raw_send(a, 4); }   /* PUBACK (QoS 2: PUBREC; we do not take part further) */
        if (m.cb) m.cb((const char *)t, tl, pay, pl, qos, tf & 1);
        return; }
    case 4:                                                         /* PUBACK */
        if (rem == 2 && m.ol && (uint16_t)(v[0] << 8 | v[1]) == m.out_pid) m.ol = 0;
        return;
    case 9: case 11:                                                /* SUBACK, UNSUBACK */
        if (rem >= 2 && (uint16_t)(v[0] << 8 | v[1]) == m.wait_pid) { m.wait_res = (tf >> 4) == 11 ? 0 : rem >= 3 && v[2] != 0x80 ? v[2] : -1; m.wait_pid = 0; }
        return;
    case 13: return;                                                /* PINGRESP */
    default: shut(1); m.state = -1; return;                         /* nothing else comes from a broker */
    }
}

static err_t q_recv(void *a, struct tcp_pcb *pcb, struct pbuf *q, err_t e)
{
    if (!q) { shut(0); return ERR_OK; }
    m.last_rx = time_ms();
    for (struct pbuf *b = q; b && m.pcb; b = b->next)
        for (unsigned i = 0; i < b->len && m.pcb; ) {
            unsigned k = b->len - i, room = sizeof m.rx - m.rl; if (k > room) k = room;
            memcpy(m.rx + m.rl, (uint8_t *)b->payload + i, k); m.rl += k; i += k;
            for (;;) {                                              /* every whole packet in the buffer */
                uint8_t tf; uint32_t rem; int hl = mqtt_fixed(m.rx, m.rl, &tf, &rem);
                if (hl < 0 || (hl > 0 && hl + rem > sizeof m.rx)) { shut(1); m.state = -1; break; }   /* malformed or larger than our buffer */
                if (hl == 0 || m.rl < hl + rem) { if (m.rl == sizeof m.rx) { shut(1); m.state = -1; } break; }
                packet(m.rx, (unsigned)hl, rem);
                if (!m.pcb) break;
                memmove(m.rx, m.rx + hl + rem, m.rl - hl - rem); m.rl -= (unsigned)hl + rem;
            }
        }
    if (m.pcb) tcp_recved(pcb, q->tot_len);
    pbuf_free(q);
    return ERR_OK;
}
static err_t q_poll(void *a, struct tcp_pcb *pcb)                  /* every lwIP slow tick (2 x TCP_TMR_INTERVAL) */
{
    uint32_t now = time_ms();
    if (m.state != 3) return ERR_OK;
    if (m.ol && now - m.out_t >= NDF_MQTT_RETX_MS) { m.out[0] |= 8; raw_send(m.out, m.ol); m.out_t = now; }       /* QoS 1 again, DUP set */
    if (now - m.last_tx >= m.keepalive * 1000u) { uint8_t p[2] = { 0xC0, 0 }; raw_send(p, 2); }                    /* PINGREQ */
    if (now - m.last_rx > m.keepalive * 1500u) { shut(1); m.state = -1; }                                          /* 1.5 keep-alive without a word: the broker is gone */
    return ERR_OK;
}
static err_t q_conn(void *a, struct tcp_pcb *pcb, err_t e) { m.state = e ? -1 : 2; return ERR_OK; }
static void q_err(void *a, err_t e) { m.pcb = 0; m.state = -1; m.ol = 0; }

int mqtt_connect(const char *url, const mqtt_cfg_t *cfg, mqtt_msg_cb cb, uint32_t timeout_ms)
{
    if (strncmp(url, "mqtt://", 7) || m.pcb) return -1;
    const char *h = url + 7; ip4_addr_t ip; unsigned b[4]; uint16_t port = 1883;
    for (int i = 0; i < 4; i++) { char *e; b[i] = (unsigned)strtoul(h, &e, 10); if (e == h || b[i] > 255) return -1; h = e + (*e == '.'); }
    IP4_ADDR(&ip, b[0], b[1], b[2], b[3]); if (*h == ':') port = (uint16_t)atoi(h + 1);
    static const mqtt_cfg_t none; if (!cfg) cfg = &none;
    memset(&m, 0, sizeof m); m.cb = cb; m.keepalive = cfg->keepalive ? cfg->keepalive : 120;
    char id[24]; if (cfg->client_id) { strncpy(id, cfg->client_id, sizeof id - 1); id[sizeof id - 1] = 0; } else { uint8_t r[3]; rng_fill(r, 3); snprintf(id, sizeof id, "esp-ndf-%02x%02x%02x", r[0], r[1], r[2]); }
    uint8_t pkt[NDF_MQTT_BUF]; unsigned n = mqtt_build_connect(pkt, sizeof pkt, id, cfg); if (!n) return -1;
    m.pcb = tcp_new(); if (!m.pcb) return -1;
    m.state = 1; tcp_recv(m.pcb, q_recv); tcp_err(m.pcb, q_err); tcp_poll(m.pcb, q_poll, 1);
    if (tcp_connect(m.pcb, &ip, port, q_conn)) { tcp_abort(m.pcb); m.pcb = 0; return -1; }
    uint32_t t0 = time_ms();
    while (m.state == 1 && time_ms() - t0 < timeout_ms) net_poll();
    if (m.state == 2 && raw_send(pkt, n)) m.state = -1;
    m.last_rx = time_ms();
    while (m.state == 2 && time_ms() - t0 < timeout_ms) net_poll();
    if (m.state != 3) { int rc = m.connack; shut(1); m.state = 0; return rc ? rc : -1; }
    return 0;
}

static int wait_ack(uint16_t pid, uint32_t timeout_ms)
{
    m.wait_pid = pid; m.wait_res = -1; uint32_t t0 = time_ms();
    while (m.wait_pid && m.state == 3 && time_ms() - t0 < timeout_ms) net_poll();
    int r = m.wait_pid ? -1 : m.wait_res; m.wait_pid = 0; return r;
}
int mqtt_subscribe(const char *topic, int qos, uint32_t timeout_ms)
{
    uint8_t p[300]; uint16_t pid = next_pid(); unsigned n = mqtt_build_subscribe(p, sizeof p, pid, topic, qos, 0);
    if (m.state != 3 || !n || raw_send(p, n)) return -1;
    return wait_ack(pid, timeout_ms);
}
int mqtt_unsubscribe(const char *topic, uint32_t timeout_ms)
{
    uint8_t p[300]; uint16_t pid = next_pid(); unsigned n = mqtt_build_subscribe(p, sizeof p, pid, topic, 0, 1);
    if (m.state != 3 || !n || raw_send(p, n)) return -1;
    return wait_ack(pid, timeout_ms);
}
int mqtt_publish(const char *topic, const void *p, unsigned n, int qos, int retain)
{
    if (m.state != 3 || (qos && m.ol)) return -1;
    if (qos) {                                                      /* kept for the retransmit */
        uint16_t pid = next_pid(); unsigned k = mqtt_build_publish(m.out, sizeof m.out, topic, p, n, 1, retain, 0, pid);
        if (!k || raw_send(m.out, k)) return -1;
        m.ol = k; m.out_pid = pid; m.out_t = time_ms(); return 0;
    }
    uint8_t b[NDF_MQTT_BUF]; unsigned k = mqtt_build_publish(b, sizeof b, topic, p, n, 0, retain, 0, 0);
    return k && !raw_send(b, k) ? 0 : -1;
}
int mqtt_inflight(void) { return m.ol != 0; }
int mqtt_connected(void) { return m.state == 3; }
void mqtt_disconnect(void) { if (m.state == 3) { uint8_t p[2] = { 0xE0, 0 }; raw_send(p, 2); } shut(0); m.state = 0; }
void mqtt_test_drop(void) { shut(1); m.state = 0; }                         /* test hook: the connection vanishes without DISCONNECT (the broker sends the will) */
