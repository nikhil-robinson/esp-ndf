/* Radio side of the GATT server: advertising, the main-loop poll, notifications. Uses ble_hw (the S3's link-layer core). */
#include "gatt.h"
#include "ble_hw.h"
#include <string.h>

static const char *dev_name;
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
    gatt_db_clear();
    dev_name = name;
    gap_chr[0] = (gatt_chr_t){ 0x2A00, GATT_P_READ, name_read, 0, 0 };
    if (gatts_add_service(0x1800, gap_chr, 1)) return -1;
    return gatts_add_service(0x1801, 0, 0);
}

int gatts_advertise(const uint8_t addr[6], uint32_t interval_us)
{
    uint8_t d[31]; unsigned n = 0, nl = strlen(dev_name);
    if (nl > 20) nl = 20;
    d[n++] = 2; d[n++] = 1; d[n++] = 6;                        /* flags: LE general discoverable, no BR/EDR */
    d[n++] = (uint8_t)(nl + 1); d[n++] = 9; memcpy(d + n, dev_name, nl); n += nl;
    ble_hw_adv_type(0);                                        /* ADV_IND: connectable */
    return ble_hw_adv(37, addr, 1, d, n, interval_us);         /* static random address */
}

static uint8_t pend[GATT_MTU]; static unsigned pend_n;         /* one response that did not fit the TX queue yet */
static int l2cap_att(const uint8_t *att, unsigned n);
int gatt_send_att(const uint8_t *att, unsigned n) { return l2cap_att(att, n); }
static int l2cap_att(const uint8_t *att, unsigned n)          /* one ATT PDU in an L2CAP frame (CID 4) */
{
    uint8_t f[27];
    if (n > 23) return -1;
    f[0] = (uint8_t)n; f[1] = 0; f[2] = 4; f[3] = 0;
    memcpy(f + 4, att, n);
    return ble_hw_send(2, f, 4 + n);
}

static void l2cap(const uint8_t *d, unsigned len)
{
    if (len < 4) return;
    unsigned l = d[0] | d[1] << 8, cid = d[2] | d[3] << 8;
    if (l + 4 > len) return;                                   /* a fragmented SDU: not used at MTU 23 */
    if (cid == 4 && l && (d[4] & 1)) gattc_rx(d + 4, l);        /* odd ATT opcodes (responses, errors, notifications) belong to the client */
    else if (cid == 4) {
        uint8_t r[GATT_MTU];
        int rl = att_handle(d + 4, l, r, sizeof r);
        if (rl > 0 && l2cap_att(r, (unsigned)rl) < 0 && !pend_n) { memcpy(pend, r, (unsigned)rl); pend_n = (unsigned)rl; }
    } else if (cid == 5 && l >= 1) {                           /* L2CAP signalling: command reject (not understood) */
        uint8_t r[10] = { 6, 0, 5, 0, 0x01, 0, 2, 0, 0, 0 };
        r[5] = l >= 2 ? d[5] : 0;                              /* the request's identifier */
        ble_hw_send(2, r, 10);
    }
}

void gatt_poll(uint32_t wait_us)
{
    static int was;
    ble_hw_pkt_t p;
    if (pend_n && l2cap_att(pend, pend_n) == 0) pend_n = 0;
    int got = wait_us ? ble_hw_wait(&p, wait_us) : ble_hw_read(&p);
    while (got) {
        if (ble_hw_connected() && !((p.hdr0 & 3) == 1 && !p.len) && !ble_hw_ll_control(&p) && (p.hdr0 & 3) == 2) l2cap(p.data, p.len);
        got = ble_hw_read(&p);
    }
    int c = ble_hw_connected();
    if (!c && was) gatt_reset();                               /* link lost: subscriptions off (ble_hw resumes advertising) */
    was = c;
}

static int send_value(uint8_t op, int bit, const gatt_chr_t *c, const uint8_t *v, unsigned n)
{
    uint8_t a[GATT_MTU];
    if (!ble_hw_connected() || n > GATT_MTU - 3) return -1;
    if (!(gatts_subscribed(c) & bit)) return -2;
    if (ble_hw_tx_free() < 3) return -4;                         /* keep slots free for ATT responses */
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
