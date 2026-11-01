/* Radio side of the GATT server: advertising, the main-loop poll, notifications. Uses ble_hw (the S3's link-layer core). */
#include "gatt.h"
#include "ble_hw.h"
#include "smp.h"
#include "rng.h"
#include "sys.h"
#include <string.h>

static const char *dev_name;
static int smp_send(const uint8_t *p, unsigned n);
static gatt_chr_t gap_chr[1];

static int name_read(uint8_t *o, uint16_t max)
{
    unsigned n = strlen(dev_name);
    if (n > max) n = max;
    memcpy(o, dev_name, n);
    return (int)n;
}

int gatts_init(const char *name)
{
    smp_init(smp_send); ble_hw_set_enc(smp_ltk, smp_encrypted);
    gatt_db_clear();
    dev_name = name;
    gap_chr[0] = (gatt_chr_t){ 0x2A00, GATT_P_READ, name_read, 0, 0 };
    if (gatts_add_service(0x1800, gap_chr, 1)) return -1;
    return gatts_add_service(0x1801, 0, 0);
}

static uint8_t adv_d[31], adv_n, adv_addr[6]; static uint32_t adv_iv_us;                  /* what gatts_advertise set up, for the address rotation */
static uint32_t rotate_ms, rotate_at;                                                    /* privacy: new resolvable private address every rotate_ms (0 = off, static address) */
int gatts_advertise(const uint8_t addr[6], uint32_t interval_us)
{
    uint8_t d[31]; unsigned n = 0, nl = strlen(dev_name);
    if (nl > 20) nl = 20;
    d[n++] = 2; d[n++] = 1; d[n++] = 6;                        /* flags: LE general discoverable, no BR/EDR */
    d[n++] = (uint8_t)(nl + 1); d[n++] = 9; memcpy(d + n, dev_name, nl); n += nl;
    ble_hw_adv_type(0);                                        /* ADV_IND: connectable */
    memcpy(adv_d, d, n); adv_n = (uint8_t)n; memcpy(adv_addr, addr, 6); adv_iv_us = interval_us;
    return ble_hw_adv(37, addr, 1, d, n, interval_us);         /* static random address */
}

/* Privacy: advertise with a resolvable private address, hash(IRK, prand) | prand, renewed every rotate_ms while no link is up (a fresh one after each link). The bonded
 * central resolves it with the IRK we gave it in the pairing; the static address stays our identity address. */
void gatts_set_privacy(uint32_t ms) { rotate_ms = ms; rotate_at = 0; }
static void rotate_adva(void)
{
    uint8_t irk[16], a[6], pr[3];
    smp_local_irk(irk);
    rng_fill(pr, 3); pr[2] = (uint8_t)((pr[2] & 0x3F) | 0x40);            /* top two bits 01: resolvable */
    smp_ah(irk, pr, a); memcpy(a + 3, pr, 3);
    ble_hw_set_adva(a);
}

static uint8_t pend[GATT_MTU]; static unsigned pend_n;         /* one response that did not fit the TX queue yet */
static int l2cap_att(const uint8_t *att, unsigned n);
int gatt_send_att(const uint8_t *att, unsigned n) { return l2cap_att(att, n); }
static int l2cap_pdu(unsigned cid, const uint8_t *att, unsigned n)          /* one PDU in an L2CAP frame, fragmented into LL PDUs of at most ble_hw_tx_max() bytes */
{
    uint8_t f[4 + GATT_MTU];
    if (n > (cid == 4 ? att_mtu() : 65u)) return -1;
    f[0] = (uint8_t)n; f[1] = (uint8_t)(n >> 8); f[2] = (uint8_t)cid; f[3] = 0;
    memcpy(f + 4, att, n);
    unsigned total = 4 + n, mx = ble_hw_tx_max(), frags = (total + mx - 1) / mx;
    if ((unsigned)ble_hw_tx_free() < frags) return -1;           /* all fragments or none */
    for (unsigned o = 0; o < total; o += mx)
        if (ble_hw_send(o ? 1 : 2, f + o, total - o > mx ? mx : total - o)) return -1;
    return 0;
}
static int l2cap_att(const uint8_t *att, unsigned n) { return l2cap_pdu(4, att, n); }
static int smp_send(const uint8_t *p, unsigned n) { return l2cap_pdu(6, p, n); }

static void l2cap(const uint8_t *d, unsigned len)
{
    if (len < 4) return;
    unsigned l = d[0] | d[1] << 8, cid = d[2] | d[3] << 8;
    if (l + 4 > len) return;
    if (cid == 4 && l && (d[4] & 1)) gattc_rx(d + 4, l);        /* odd ATT opcodes (responses, errors, notifications) belong to the client */
    else if (cid == 4) {
        uint8_t r[GATT_MTU];
        int rl = att_handle(d + 4, l, r, sizeof r);
        if (rl > 0 && l2cap_att(r, (unsigned)rl) < 0 && !pend_n) { memcpy(pend, r, (unsigned)rl); pend_n = (unsigned)rl; }
    } else if (cid == 6) smp_rx(d + 4, l);
    else if (cid == 5 && l >= 1) {                           /* L2CAP signalling: command reject (not understood) */
        uint8_t r[10] = { 6, 0, 5, 0, 0x01, 0, 2, 0, 0, 0 };
        r[5] = l >= 2 ? d[5] : 0;                              /* the request's identifier */
        ble_hw_send(2, r, 10);
    }
}

/* LL data PDUs to L2CAP frames: llid 2 starts one, llid 1 continues it (reassembled up to one frame of 4 + GATT_MTU bytes) */
static uint8_t rx_f[4 + GATT_MTU]; static unsigned rx_have, rx_need;
static void ll_data(const ble_hw_pkt_t *p)
{
    unsigned llid = p->hdr0 & 3;
    if (llid == 2) {
        if (p->len < 4) { rx_need = 0; return; }
        unsigned need = 4 + (p->data[0] | p->data[1] << 8);
        if (p->len >= need) { rx_need = 0; l2cap(p->data, p->len); return; }
        if (need > sizeof rx_f) { rx_need = 0; return; }       /* longer than we accept: drop the frame */
        memcpy(rx_f, p->data, p->len); rx_have = p->len; rx_need = need;
    } else if (llid == 1 && rx_need && p->len) {
        if (rx_have + p->len > rx_need) { rx_need = 0; return; }
        memcpy(rx_f + rx_have, p->data, p->len); rx_have += p->len;
        if (rx_have == rx_need) { rx_need = 0; l2cap(rx_f, rx_have); }
    }
}

void gatt_poll(uint32_t wait_us)
{
    static int was;
    ble_hw_pkt_t p;
    if (pend_n && l2cap_att(pend, pend_n) == 0) pend_n = 0;
    int got = wait_us ? ble_hw_wait(&p, wait_us) : ble_hw_read(&p);
    while (got) {
        if (ble_hw_connected() && !((p.hdr0 & 3) == 1 && !p.len) && !ble_hw_ll_control(&p) && ((p.hdr0 & 3) == 2 || (p.hdr0 & 3) == 1)) ll_data(&p);
        got = ble_hw_read(&p);
    }
    int c = ble_hw_connected();
    if (rotate_ms && !c) {                                     /* privacy: renew the advertising address */
        uint32_t now = (uint32_t)(time_us() / 1000);
        if (was || !rotate_at || (int32_t)(now - rotate_at) >= 0) { rotate_adva(); rotate_at = now + rotate_ms; }
    }
    if (c && !was) { uint8_t peer[7], own[7]; ble_hw_conn_addrs(peer, own); smp_connected(peer, own); }
    if (!c && was) { gatt_reset(); rx_need = 0; smp_disconnected(); }                               /* link lost: subscriptions off (ble_hw resumes advertising) */
    was = c;
}

static int send_value(uint8_t op, int bit, const gatt_chr_t *c, const uint8_t *v, unsigned n)
{
    uint8_t a[GATT_MTU];
    if (!ble_hw_connected() || n > att_mtu() - 3) return -1;
    if (!(gatts_subscribed(c) & bit)) return -2;
    if (ble_hw_tx_free() < 3 + (int)((4 + 3 + n) / ble_hw_tx_max())) return -4;       /* keep slots free for ATT responses */
    a[0] = op; a[1] = (uint8_t)c->handle; a[2] = (uint8_t)(c->handle >> 8); memcpy(a + 3, v, n);
    return l2cap_att(a, 3 + n) < 0 ? -3 : 0;
}
int gatts_notify(const gatt_chr_t *c, const uint8_t *v, unsigned n) { return send_value(0x1B, 1, c, v, n); }
int gatts_indicate(const gatt_chr_t *c, const uint8_t *v, unsigned n)
{
    if (att_indication_pending()) return -4;
    int r = send_value(0x1D, 2, c, v, n);
    if (!r) att_indication_start();
    return r;
}
