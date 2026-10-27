/* GATT client over the connection we already have (ATT client, Core spec vol 3 part F/G): discovery, read, write, subscribe.
 * One request in flight; every call runs gatt_poll while it waits. 16-bit UUIDs (128-bit entries are listed with uuid16 = 0). */
#include "gatt.h"
#include <string.h>
#ifdef HOST_FUZZ                                        /* tools/fuzz/fuzz_gattc.c supplies these */
int ble_hw_connected(void);
uint64_t time_us(void);
#else
#include "ble_hw.h"
#include "sys.h"
#endif

#define TIMEOUT_US 5000000u
#define ERR(c) (-(0x100 + (c)))

static uint8_t rsp[GATT_MTU]; static volatile unsigned rsp_n;           /* the answer to the request in flight */
static struct { uint16_t handle; void (*cb)(uint16_t, const uint8_t *, unsigned); } subs[8];

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

void gattc_rx(const uint8_t *p, unsigned n)
{
    if (!n) return;
    if ((p[0] == 0x1B || p[0] == 0x1D) && n >= 3) {                   /* notification / indication: dispatch, confirm */
        uint16_t h = u16(p + 1);
        for (unsigned i = 0; i < 8; i++) if (subs[i].cb && subs[i].handle == h) { subs[i].cb(h, p + 3, n - 3); break; }
        if (p[0] == 0x1D) { uint8_t c = 0x1E; gatt_send_att(&c, 1); }
        return;
    }
    if (n <= sizeof rsp && !rsp_n) { memcpy(rsp, p, n); rsp_n = n; }       /* a response: one at a time */
}

/* send req, wait for its response (opcode want or an error response for req[0]): length in rsp, or a negative code */
static int transact(const uint8_t *req, unsigned n, uint8_t want)
{
    if (!ble_hw_connected()) return -1;
    rsp_n = 0;
    uint64_t t0 = time_us();
    while (gatt_send_att(req, n) < 0) { gatt_poll(1000); if (time_us() - t0 > TIMEOUT_US) return -1; }
    while (time_us() - t0 < TIMEOUT_US) {
        gatt_poll(2000);
        if (!ble_hw_connected()) return -1;
        if (rsp_n) {
            if (rsp[0] == want) return (int)rsp_n;
            if (rsp[0] == 0x01 && rsp_n >= 5 && rsp[1] == req[0]) return ERR(rsp[4]);
            rsp_n = 0;                                                      /* not ours: wait on */
        }
    }
    return -1;
}

int gattc_discover(gattc_svc_t *s, unsigned max, unsigned *count)
{
    unsigned n = 0; uint16_t start = 1;
    while (start) {
        uint8_t q[7] = { 0x10, (uint8_t)start, (uint8_t)(start >> 8), 0xFF, 0xFF, 0x00, 0x28 };
        int r = transact(q, 7, 0x11);
        if (r == ERR(0x0A)) break;                                           /* attribute not found: done */
        if (r < 0) return r;
        unsigned el = rsp[1];
        if (el != 6 && el != 20) return -1;
        uint16_t last = 0;
        for (unsigned o = 2; o + el <= (unsigned)r; o += el) {
            if (n < max) s[n] = (gattc_svc_t){ u16(rsp + o), u16(rsp + o + 2), el == 6 ? u16(rsp + o + 4) : 0 };
            n++; last = u16(rsp + o + 2);
        }
        start = (last && last != 0xFFFF && last >= start) ? (uint16_t)(last + 1) : 0;
    }
    *count = n < max ? n : max;
    return (int)*count;
}

int gattc_chars(const gattc_svc_t *sv, gattc_chr_t *c, unsigned max, unsigned *count)
{
    unsigned n = 0; uint16_t start = sv->svc_start;
    while (start && start <= sv->svc_end) {
        uint8_t q[7] = { 0x08, (uint8_t)start, (uint8_t)(start >> 8), (uint8_t)sv->svc_end, (uint8_t)(sv->svc_end >> 8), 0x03, 0x28 };
        int r = transact(q, 7, 0x09);
        if (r == ERR(0x0A)) break;
        if (r < 0) return r;
        unsigned el = rsp[1];
        if (el != 7 && el != 21) return -1;
        uint16_t last = 0;
        for (unsigned o = 2; o + el <= (unsigned)r; o += el) {
            gattc_chr_t x = { u16(rsp + o), u16(rsp + o + 3), 0, rsp[o + 2], el == 7 ? u16(rsp + o + 5) : 0 };
            if (n < max) c[n] = x;
            n++; last = x.decl;
        }
        start = (last >= start && last < 0xFFFF) ? (uint16_t)(last + 1) : 0;
    }
    unsigned cnt = n < max ? n : max;
    for (unsigned i = 0; i < cnt; i++) {                                     /* CCCD: Find Information between this value and the next declaration */
        uint16_t from = (uint16_t)(c[i].value + 1), to = i + 1 < cnt ? (uint16_t)(c[i + 1].decl - 1) : sv->svc_end;
        while (from <= to) {
            uint8_t q[5] = { 0x04, (uint8_t)from, (uint8_t)(from >> 8), (uint8_t)to, (uint8_t)(to >> 8) };
            int r = transact(q, 5, 0x05);
            if (r == ERR(0x0A)) break;
            if (r < 0) return r;
            if (rsp[1] != 1) break;                                           /* format 2: 128-bit UUIDs only */
            uint16_t last = from;
            for (unsigned o = 2; o + 4 <= (unsigned)r; o += 4) { last = u16(rsp + o); if (u16(rsp + o + 2) == 0x2902) c[i].cccd = last; }
            from = last < 0xFFFF ? (uint16_t)(last + 1) : 0xFFFF;
            if (!from || last >= to) break;
        }
    }
    *count = cnt;
    return (int)cnt;
}

int gattc_read(uint16_t h, uint8_t *out, unsigned max)
{
    unsigned have = 0;
    for (;;) {
        uint8_t q[5]; unsigned ql;
        if (!have) { q[0] = 0x0A; ql = 3; } else { q[0] = 0x0C; q[3] = (uint8_t)have; q[4] = (uint8_t)(have >> 8); ql = 5; }
        q[1] = (uint8_t)h; q[2] = (uint8_t)(h >> 8);
        int r = transact(q, ql, have ? 0x0D : 0x0B);
        if (r < 0) return have && r == ERR(0x07) ? (int)have : r;           /* offset past the end after a full-PDU value: done */
        unsigned k = (unsigned)r - 1;
        if (have + k > max) k = max - have;
        memcpy(out + have, rsp + 1, k); have += k;
        if ((unsigned)r < GATT_MTU || have >= max) return (int)have;       /* a short PDU ends the value */
    }
}

int gattc_write(uint16_t h, const uint8_t *v, unsigned n, int with_response)
{
    uint8_t q[GATT_MTU];
    if (n > GATT_MTU - 3) return -1;
    q[0] = with_response ? 0x12 : 0x52; q[1] = (uint8_t)h; q[2] = (uint8_t)(h >> 8); memcpy(q + 3, v, n);
    if (!with_response) return ble_hw_connected() && gatt_send_att(q, 3 + n) == 0 ? 0 : -1;
    int r = transact(q, 3 + n, 0x13);
    return r < 0 ? r : 0;
}

int gattc_subscribe(const gattc_chr_t *c, int indicate, void (*cb)(uint16_t, const uint8_t *, unsigned))
{
    if (!c->cccd) return ERR(0x0A);
    unsigned i = 0;
    while (i < 8 && subs[i].cb && subs[i].handle != c->value) i++;
    if (i == 8) return -1;
    subs[i].handle = c->value; subs[i].cb = cb;                              /* before the write: the first notification may follow it at once */
    uint8_t v[2] = { (uint8_t)(indicate ? 2 : 1), 0 };
    int r = gattc_write(c->cccd, v, 2, 1);
    if (r < 0) subs[i].cb = 0;
    return r;
}
