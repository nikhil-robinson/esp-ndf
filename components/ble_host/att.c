/* ATT server and attribute table (Core spec vol 3 parts F and G): 16-bit UUIDs, MTU 23 up to NDF_GATT_MTU after Exchange MTU, no prepare/execute write.
 * Pure code (no radio): att_handle() takes one ATT PDU and builds the response. Every length is checked first. */
#include "gatt.h"
#include <string.h>

enum { K_SVC, K_DECL, K_VAL, K_CCCD };
typedef struct { uint8_t kind; uint16_t uuid, end; gatt_chr_t *chr; uint16_t cccd[BLE_NCONN]; } attr_t;      /* subscriptions per link */
static attr_t db[NDF_GATT_MAX_ATTRS];
static unsigned nattr;
static uint8_t ind_pending;
static uint16_t mtus[BLE_NCONN] = { 23, 23 };                 /* ATT MTU per link */
static unsigned att_link;
void att_set_link(int k) { att_link = (unsigned)(k >= 0 && k < BLE_NCONN ? k : 0); }       /* the link the next call works on (the host sets it from ble_hw_select) */
#define mtu (mtus[att_link])

enum { E_INVALID_HANDLE = 1, E_READ_NOT_PERM = 2, E_WRITE_NOT_PERM = 3, E_INVALID_PDU = 4, E_NOT_SUPPORTED = 6, E_INVALID_OFFSET = 7,
       E_NOT_FOUND = 0x0A, E_INVALID_LEN = 0x0D, E_UNSUP_GROUP = 0x10 };

void gatt_db_clear(void) { memset(db, 0, sizeof db); nattr = 0; ind_pending = 0; for (int k = 0; k < BLE_NCONN; k++) mtus[k] = 23; }
void gatt_reset(void) { for (int k = 0; k < BLE_NCONN; k++) gatt_link_reset(k); ind_pending = 0; }
void gatt_link_reset(int k) { if (k < 0 || k >= BLE_NCONN) return; for (unsigned i = 0; i < nattr; i++) db[i].cccd[k] = 0; mtus[k] = 23; }
unsigned att_mtu(void) { return mtu; }
int  att_indication_pending(void) { return ind_pending; }
void att_indication_start(void) { ind_pending = 1; }

int gatts_add_service(uint16_t uuid16, gatt_chr_t *chrs, unsigned n)
{
    unsigned need = 1;
    for (unsigned i = 0; i < n; i++) need += 2 + ((chrs[i].props & (GATT_P_NOTIFY | GATT_P_INDICATE)) != 0);
    if (nattr + need > NDF_GATT_MAX_ATTRS) return -1;
    unsigned svc = nattr;
    db[nattr++] = (attr_t){ K_SVC, uuid16, 0, 0, { 0 } };
    for (unsigned i = 0; i < n; i++) {
        gatt_chr_t *c = &chrs[i];
        db[nattr++] = (attr_t){ K_DECL, 0x2803, 0, c, { 0 } };
        db[nattr++] = (attr_t){ K_VAL, c->uuid, 0, c, { 0 } };
        c->handle = (uint16_t)nattr;                              /* handle = index + 1 */
        if (c->props & (GATT_P_NOTIFY | GATT_P_INDICATE)) db[nattr++] = (attr_t){ K_CCCD, 0x2902, 0, c, { 0 } };
    }
    db[svc].end = (uint16_t)nattr;
    return 0;
}

int att_cccd(const gatt_chr_t *c)
{
    if (!(c->props & (GATT_P_NOTIFY | GATT_P_INDICATE)) || c->handle == 0 || c->handle >= nattr) return 0;
    return db[c->handle].cccd[att_link];                                    /* the CCCD is the attribute right after the value (index = handle) */
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static int err(uint8_t *r, uint8_t op, uint16_t h, uint8_t e) { r[0] = 0x01; r[1] = op; put16(r + 2, h); r[4] = e; return 5; }
static uint16_t type_of(const attr_t *a) { return a->kind == K_SVC ? 0x2800 : a->uuid; }      /* K_DECL / K_CCCD carry 0x2803 / 0x2902 */

/* value of attribute a into out (<= max bytes): length, or -(ATT error) */
static int value(const attr_t *a, uint8_t *out, unsigned max)
{
    switch (a->kind) {
    case K_SVC:  put16(out, a->uuid); return 2;
    case K_DECL: out[0] = a->chr->props; put16(out + 1, a->chr->handle); put16(out + 3, a->chr->uuid); return 5;
    case K_CCCD: put16(out, a->cccd[att_link]); return 2;
    default: {
        if (!(a->chr->props & GATT_P_READ) || !a->chr->read) return -E_READ_NOT_PERM;
        int n = a->chr->read(out, (uint16_t)max);
        return n > (int)max ? (int)max : n;
    }
    }
}

static int write_attr(attr_t *a, const uint8_t *d, unsigned n)           /* 0 ok or an ATT error code */
{
    if (a->kind == K_CCCD) {
        if (n != 2) return E_INVALID_LEN;
        uint16_t v = u16(d), allowed = (uint16_t)(((a->chr->props & GATT_P_NOTIFY) ? 1 : 0) | ((a->chr->props & GATT_P_INDICATE) ? 2 : 0));
        if (v & ~allowed) return E_WRITE_NOT_PERM;
        a->cccd[att_link] = v;
        return 0;
    }
    if (a->kind != K_VAL || !(a->chr->props & (GATT_P_WRITE | GATT_P_WRITE_NR)) || !a->chr->write) return E_WRITE_NOT_PERM;
    int rc = a->chr->write(d, (uint16_t)n);
    return rc < 0 ? -rc : 0;
}

int att_handle(const uint8_t *p, unsigned n, uint8_t *r, unsigned max)
{
    if (!n || max < GATT_MTU) return 0;
    const uint8_t op = p[0];
    const unsigned room = mtu;                                            /* bytes of one response */
    uint8_t v[GATT_MTU];
    switch (op) {
    case 0x02:                                                            /* Exchange MTU */
        if (n != 3) return err(r, op, 0, E_INVALID_PDU);
        { unsigned c = u16(p + 1); mtu = (uint16_t)(c < 23 ? 23 : c > GATT_MTU ? GATT_MTU : c); }       /* the smaller of the two receive MTUs */
        r[0] = 0x03; put16(r + 1, GATT_MTU); return 3;

    case 0x04: case 0x06: case 0x08: case 0x10: {                          /* handle-range requests */
        if (n < 5) return err(r, op, 0, E_INVALID_PDU);
        uint16_t s = u16(p + 1), e = u16(p + 3);
        if (!s || s > e) return err(r, op, s, E_INVALID_HANDLE);
        if (e > nattr) e = (uint16_t)nattr;
        unsigned o = 0;
        if (op == 0x04) {                                                  /* Find Information */
            if (n != 5) return err(r, op, 0, E_INVALID_PDU);
            r[0] = 0x05; r[1] = 1; o = 2;
            for (unsigned h = s; h <= e && o + 4 <= room; h++) { put16(r + o, (uint16_t)h); put16(r + o + 2, type_of(&db[h - 1])); o += 4; }
            return o > 2 ? (int)o : err(r, op, s, E_NOT_FOUND);
        }
        if (op == 0x06) {                                                  /* Find By Type Value: primary service by 16-bit UUID */
            if (n != 9 || u16(p + 5) != 0x2800) return err(r, op, s, n < 9 ? E_INVALID_PDU : E_NOT_FOUND);
            r[0] = 0x07; o = 1;
            for (unsigned h = s; h <= e && o + 4 <= room; h++)
                if (db[h - 1].kind == K_SVC && db[h - 1].uuid == u16(p + 7)) { put16(r + o, (uint16_t)h); put16(r + o + 2, db[h - 1].end); o += 4; }
            return o > 1 ? (int)o : err(r, op, s, E_NOT_FOUND);
        }
        if (n != 7 && n != 21) return err(r, op, s, E_INVALID_PDU);        /* Read By Type / Read By Group Type */
        if (n == 21) return err(r, op, s, op == 0x10 ? E_UNSUP_GROUP : E_NOT_FOUND);     /* 128-bit UUIDs: not served */
        uint16_t t = u16(p + 5);
        if (op == 0x10) {
            if (t != 0x2800) return err(r, op, s, E_UNSUP_GROUP);
            r[0] = 0x11; r[1] = 6; o = 2;
            for (unsigned h = s; h <= e && o + 6 <= room; h++)
                if (db[h - 1].kind == K_SVC) { put16(r + o, (uint16_t)h); put16(r + o + 2, db[h - 1].end); put16(r + o + 4, db[h - 1].uuid); o += 6; }
            return o > 2 ? (int)o : err(r, op, s, E_NOT_FOUND);
        }
        r[0] = 0x09; o = 2; unsigned elen = 0;
        for (unsigned h = s; h <= e; h++) {
            const attr_t *a = &db[h - 1];
            if (type_of(a) != t) continue;
            int k = value(a, v, room - 6);
            if (k < 0) { if (o == 2) return err(r, op, (uint16_t)h, (uint8_t)-k); break; }
            if (elen && (unsigned)k + 2 != elen) break;                    /* every entry has the same length */
            if (o + 2 + (unsigned)k > room) break;
            elen = (unsigned)k + 2; r[1] = (uint8_t)elen;
            put16(r + o, (uint16_t)h); memcpy(r + o + 2, v, (unsigned)k); o += elen;
        }
        return o > 2 ? (int)o : err(r, op, s, E_NOT_FOUND);
    }

    case 0x0A: case 0x0C: {                                                /* Read / Read Blob */
        if (n != (op == 0x0A ? 3u : 5u)) return err(r, op, 0, E_INVALID_PDU);
        uint16_t h = u16(p + 1), off = op == 0x0C ? u16(p + 3) : 0;
        if (!h || h > nattr) return err(r, op, h, E_INVALID_HANDLE);
        int k = value(&db[h - 1], v, sizeof v);
        if (k < 0) return err(r, op, h, (uint8_t)-k);
        if (off > (unsigned)k) return err(r, op, h, E_INVALID_OFFSET);
        unsigned c = (unsigned)k - off; if (c > room - 1) c = room - 1;
        r[0] = op == 0x0A ? 0x0B : 0x0D; memcpy(r + 1, v + off, c);
        return (int)(1 + c);
    }

    case 0x12: case 0x52: {                                                /* Write Request / Write Command */
        if (n < 3) return op == 0x52 ? 0 : err(r, op, 0, E_INVALID_PDU);
        uint16_t h = u16(p + 1);
        if (!h || h > nattr) return op == 0x52 ? 0 : err(r, op, h, E_INVALID_HANDLE);
        int e = write_attr(&db[h - 1], p + 3, n - 3);
        if (op == 0x52) return 0;
        if (e) return err(r, op, h, (uint8_t)e);
        r[0] = 0x13; return 1;
    }

    case 0x1E: ind_pending = 0; return 0;                                  /* Handle Value Confirmation */
    case 0x16: case 0x18: return err(r, op, 0, E_NOT_SUPPORTED);           /* Prepare / Execute Write: values are at most one PDU */
    default:
        if (op & 0x40) return 0;                                           /* unknown commands get no answer */
        return err(r, op, 0, E_NOT_SUPPORTED);
    }
}
