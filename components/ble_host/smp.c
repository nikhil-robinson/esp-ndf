/* Security Manager, responder side (Core spec vol 3 part H): LE Secure Connections with the Just Works association model, key distribution of the identity keys and a small
 * bond store. Order of a pairing as we see it: Pairing Request -> Pairing Response; Public Key (initiator) -> Public Key + Pairing Confirm; Pairing Random -> Pairing Random;
 * DHKey Check -> DHKey Check; the link layer then encrypts with the long term key and identity information is exchanged. Every PDU is length checked and a PDU
 * out of order ends the pairing with a Pairing Failed. */
#include "smp.h"
#include "p256.h"
#include "rng.h"
#include <string.h>

enum { OP_REQ = 1, OP_RSP = 2, OP_CONFIRM = 3, OP_RANDOM = 4, OP_FAILED = 5, OP_ENC_INFO = 6, OP_CENTRAL_ID = 7, OP_ID_INFO = 8, OP_ID_ADDR = 9, OP_SIGN = 0x0A, OP_SECREQ = 0x0B, OP_PUBKEY = 0x0C, OP_DHCHECK = 0x0D };
enum { ST_IDLE, ST_RSP, ST_CONFIRM, ST_RANDOM, ST_CHECKED, ST_KEYS };
enum { E_REPEATED = 0x09, E_NOT_SUPPORTED = 0x05, E_KEYSIZE = 0x06, E_UNSPECIFIED = 0x08, E_INVALID = 0x0A, E_DHKEY = 0x0B };

static struct {
    uint8_t state, encrypted, ltk_ready, key_size, init_dist, resp_dist, got_irk, got_addr;
    uint8_t peer[7], own[7], req[7], rsp[7];
    uint8_t priv[32], pkb[64], pka[64];              /* private key big-endian (p256.h), public keys as on the air (X, Y least significant octet first) */
    uint8_t na[16], nb[16], dhkey[32], mackey[16], ltk[16], irk[16], id_addr[7];
} sm;
static smp_bond_t bonds[SMP_BONDS];
static uint8_t local_irk[16], have_irk;
static int (*send_cb)(const uint8_t *, unsigned);
static int (*load_cb)(void *, unsigned *), (*save_cb)(const void *, unsigned);

static void rev(uint8_t *o, const uint8_t *i, unsigned n) { for (unsigned k = 0; k < n; k++) o[k] = i[n - 1 - k]; }
static void wipe(void *p, unsigned n) { volatile uint8_t *q = p; while (n--) *q++ = 0; }
static int ct_eq(const uint8_t *a, const uint8_t *b, unsigned n) { uint8_t d = 0; while (n--) d |= *a++ ^ *b++; return d == 0; }
static void tx(const uint8_t *p, unsigned n) { if (send_cb) send_cb(p, n); }

typedef struct { uint8_t irk[16], have; smp_bond_t b[SMP_BONDS]; } blob_t;
static void store(void)
{
    blob_t b; memset(&b, 0, sizeof b); memcpy(b.irk, local_irk, 16); b.have = have_irk; memcpy(b.b, bonds, sizeof bonds);
    if (save_cb) save_cb(&b, sizeof b);
}
void smp_init(int (*send)(const uint8_t *, unsigned)) { send_cb = send; memset(&sm, 0, sizeof sm); }
void smp_set_store(int (*load)(void *, unsigned *), int (*save)(const void *, unsigned))
{
    load_cb = load; save_cb = save;
    blob_t b; unsigned n = sizeof b;
    if (load && !load(&b, &n) && n == sizeof b) { memcpy(local_irk, b.irk, 16); have_irk = b.have; memcpy(bonds, b.b, sizeof bonds); }
}
void smp_local_irk(uint8_t irk[16]) { if (!have_irk) { rng_fill(local_irk, 16); have_irk = 1; store(); } memcpy(irk, local_irk, 16); }
unsigned smp_bonds(smp_bond_t *out, unsigned max) { unsigned n = 0; for (unsigned i = 0; i < SMP_BONDS && n < max; i++) if (bonds[i].used) out[n++] = bonds[i]; return n; }
void smp_bonds_clear(void) { wipe(bonds, sizeof bonds); store(); }
int smp_is_encrypted(void) { return sm.encrypted; }

void smp_connected(const uint8_t peer[7], const uint8_t own[7])
{
    wipe(&sm, sizeof sm); memcpy(sm.peer, peer, 7); memcpy(sm.own, own, 7);
}
void smp_disconnected(void) { wipe(&sm, sizeof sm); }

static void fail(uint8_t reason)
{
    uint8_t p[2] = { OP_FAILED, reason }, peer[7], own[7];
    tx(p, 2); memcpy(peer, sm.peer, 7); memcpy(own, sm.own, 7);
    wipe(&sm, sizeof sm); memcpy(sm.peer, peer, 7); memcpy(sm.own, own, 7);                       /* the pairing is over, the link stays */
}

/* the bond for the connected peer: its address as given, or a resolvable private address resolved with a stored IRK */
static smp_bond_t *bond_for_peer(void)
{
    for (unsigned i = 0; i < SMP_BONDS; i++) if (bonds[i].used && !memcmp(bonds[i].addr, sm.peer, 7)) return &bonds[i];
    if (sm.peer[0] == 1 && (sm.peer[6] & 0xC0) == 0x40) {                          /* random type, top bits 01: resolvable private address: hash in the low 3 octets, prand above */
        for (unsigned i = 0; i < SMP_BONDS; i++) {
            uint8_t h[3];
            if (!bonds[i].used || !bonds[i].has_irk) continue;
            smp_ah(bonds[i].irk, sm.peer + 4, h);
            if (ct_eq(h, sm.peer + 1, 3)) return &bonds[i];
        }
    }
    return 0;
}

int smp_ltk(const uint8_t rand[8], uint16_t ediv, uint8_t ltk[16])
{
    static const uint8_t zero[8];
    if (ediv || memcmp(rand, zero, 8)) return -1;                                  /* Secure Connections keys are looked up by the link, EDIV and Rand are zero */
    if (sm.ltk_ready) { memcpy(ltk, sm.ltk, 16); return 0; }
    smp_bond_t *b = bond_for_peer();
    if (!b) return -1;
    memcpy(ltk, b->ltk, 16); return 0;
}

static void save_bond(void)
{
    smp_bond_t nb; memset(&nb, 0, sizeof nb);
    nb.used = 1; nb.key_size = 16; memcpy(nb.ltk, sm.ltk, 16);
    memcpy(nb.addr, sm.got_addr ? sm.id_addr : sm.peer, 7);
    if (sm.got_irk) { nb.has_irk = 1; memcpy(nb.irk, sm.irk, 16); }
    unsigned slot = SMP_BONDS;
    for (unsigned i = 0; i < SMP_BONDS; i++) if (bonds[i].used && !memcmp(bonds[i].addr, nb.addr, 7)) slot = i;      /* the same device pairs again: replace */
    for (unsigned i = 0; i < SMP_BONDS && slot == SMP_BONDS; i++) if (!bonds[i].used) slot = i;
    if (slot == SMP_BONDS) { memmove(bonds, bonds + 1, sizeof bonds - sizeof bonds[0]); slot = SMP_BONDS - 1; }       /* full: the oldest goes */
    bonds[slot] = nb; store();
}

void smp_encrypted(int on)
{
    sm.encrypted = (uint8_t)on;
    if (!on || sm.state != ST_CHECKED) return;
    sm.state = ST_KEYS;
    if (sm.resp_dist & 2) {                                                         /* our identity keys first (the responder distributes first) */
        uint8_t irk[16], p[17], a[8];
        smp_local_irk(irk);
        p[0] = OP_ID_INFO; memcpy(p + 1, irk, 16); tx(p, 17);
        a[0] = OP_ID_ADDR; memcpy(a + 1, sm.own, 7); tx(a, 8);
    }
    if (!(sm.init_dist & 2)) { save_bond(); sm.state = ST_IDLE; }                    /* nothing more to wait for */
}

void smp_rx(const uint8_t *p, unsigned n)
{
    if (!n) return;
    switch (p[0]) {
    case OP_REQ: {
        if (n != 7) { fail(E_INVALID); return; }
        if (sm.state != ST_IDLE) { fail(E_UNSPECIFIED); return; }
        if (!(p[3] & 0x08)) { fail(E_NOT_SUPPORTED); return; }                      /* legacy pairing only: not supported */
        if (p[4] < 16) { fail(E_KEYSIZE); return; }
        if (p[2] > 1) { fail(E_INVALID); return; }
        memcpy(sm.req, p, 7);
        sm.init_dist = p[5] & 2; sm.resp_dist = p[6] & 2;
        uint8_t r[7] = { OP_RSP, 0x03, 0x00, (uint8_t)(0x08 | (p[3] & 1)), 16, sm.init_dist, sm.resp_dist };   /* NoInputNoOutput, no OOB, SC (+ bonding if asked), 16-byte key */
        memcpy(sm.rsp, r, 7);
        if (p256_keygen(sm.priv, sm.pkb)) { fail(E_UNSPECIFIED); return; }
        uint8_t x[32], y[32]; memcpy(x, sm.pkb, 32); memcpy(y, sm.pkb + 32, 32);
        rev(sm.pkb, x, 32); rev(sm.pkb + 32, y, 32);                                 /* to the air order */
        sm.state = ST_RSP; tx(r, 7); return;
    }
    case OP_PUBKEY: {
        if (n != 65) { fail(E_INVALID); return; }
        if (sm.state != ST_RSP) { fail(E_UNSPECIFIED); return; }
        memcpy(sm.pka, p + 1, 64);
        uint8_t peer_be[64], d[32], pb[65];
        rev(peer_be, sm.pka, 32); rev(peer_be + 32, sm.pka + 32, 32);
        if (!memcmp(sm.pka, sm.pkb, 64) || p256_check_pub(peer_be) || p256_ecdh(d, sm.priv, peer_be)) { fail(E_INVALID); return; }
        rev(sm.dhkey, d, 32); wipe(d, 32); wipe(sm.priv, 32);                        /* the secret is not needed again */
        pb[0] = OP_PUBKEY; memcpy(pb + 1, sm.pkb, 64); tx(pb, 65);
        rng_fill(sm.nb, 16);
        uint8_t c[17]; c[0] = OP_CONFIRM; smp_f4(sm.pkb, sm.pka, sm.nb, 0, c + 1);   /* Cb = f4(PKb, PKa, Nb, 0) */
        tx(c, 17); sm.state = ST_CONFIRM; return;
    }
    case OP_RANDOM: {
        if (n != 17) { fail(E_INVALID); return; }
        if (sm.state != ST_CONFIRM) { fail(E_UNSPECIFIED); return; }
        memcpy(sm.na, p + 1, 16);
        uint8_t r[17]; r[0] = OP_RANDOM; memcpy(r + 1, sm.nb, 16); tx(r, 17);
        smp_f5(sm.dhkey, sm.na, sm.nb, sm.peer, sm.own, sm.mackey, sm.ltk);          /* A = initiator, B = us */
        sm.state = ST_RANDOM; return;
    }
    case OP_DHCHECK: {
        if (n != 17) { fail(E_INVALID); return; }
        if (sm.state != ST_RANDOM) { fail(E_UNSPECIFIED); return; }
        uint8_t zero[16] = { 0 }, ea[16], eb[17];
        smp_f6(sm.mackey, sm.na, sm.nb, zero, sm.req + 1, sm.peer, sm.own, ea);      /* Ea = f6(MacKey, Na, Nb, 0, IOcapA, A, B) */
        if (!ct_eq(ea, p + 1, 16)) { fail(E_DHKEY); return; }
        eb[0] = OP_DHCHECK; smp_f6(sm.mackey, sm.nb, sm.na, zero, sm.rsp + 1, sm.own, sm.peer, eb + 1);
        tx(eb, 17); sm.ltk_ready = 1; sm.state = ST_CHECKED; return;
    }
    case OP_ID_INFO:
        if (n != 17 || sm.state != ST_KEYS) { fail(E_INVALID); return; }
        memcpy(sm.irk, p + 1, 16); sm.got_irk = 1; return;
    case OP_ID_ADDR:
        if (n != 8 || sm.state != ST_KEYS) { fail(E_INVALID); return; }
        memcpy(sm.id_addr, p + 1, 7); sm.got_addr = 1;
        save_bond(); sm.state = ST_IDLE; return;
    case OP_FAILED: wipe(&sm.state, 1); sm.ltk_ready = 0; return;
    case OP_CONFIRM: case OP_ENC_INFO: case OP_CENTRAL_ID: case OP_SIGN: case OP_SECREQ: default:
        if (n >= 1 && p[0] != OP_FAILED) { uint8_t f[2] = { OP_FAILED, E_NOT_SUPPORTED }; tx(f, 2); }
        return;
    }
}
