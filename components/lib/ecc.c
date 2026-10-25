/* Software ECC on 8 x 32-bit limbs with Montgomery arithmetic: NIST P-256 (mod p and n, Jacobian, a = -3; p256.h) and
 * X25519 (RFC 7748 ladder mod 2^255-19; x25519.h). */
#include "p256.h"
#include "x25519.h"
#include "rng.h"
#include <string.h>
#include <strings.h>
#include "sha.h"

typedef uint32_t fe[8];                       /* little-endian limbs */
typedef struct { const uint32_t *m; uint32_t mp; fe rr, one; } mod_t;   /* modulus, -m^-1 mod 2^32, R^2, R (mod m) */
typedef struct { fe x, y, z; } pt;            /* Jacobian, coordinates in Montgomery form; z = 0 is infinity */

static const fe P = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0, 1, 0xFFFFFFFF };
static const fe N = { 0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0xFFFFFFFF };
static const uint8_t B_BE[32] = { 0x5A,0xC6,0x35,0xD8,0xAA,0x3A,0x93,0xE7,0xB3,0xEB,0xBD,0x55,0x76,0x98,0x86,0xBC,
                                  0x65,0x1D,0x06,0xB0,0xCC,0x53,0xB0,0xF6,0x3B,0xCE,0x3C,0x3E,0x27,0xD2,0x60,0x4B };
static const uint8_t GX_BE[32] = { 0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,
                                   0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96 };
static const uint8_t GY_BE[32] = { 0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,
                                   0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5 };
static mod_t mp_, mn_;
static fe b_m;                                 /* b in Montgomery form mod p */
static pt G;
static int ready;

static void from_be(fe r, const uint8_t *b) { for (int i = 0; i < 8; i++) r[i] = (uint32_t)b[31 - 4*i] | b[30 - 4*i] << 8 | b[29 - 4*i] << 16 | (uint32_t)b[28 - 4*i] << 24; }
static void to_be(uint8_t *b, const fe a) { for (int i = 0; i < 8; i++) { b[31 - 4*i] = a[i]; b[30 - 4*i] = a[i] >> 8; b[29 - 4*i] = a[i] >> 16; b[28 - 4*i] = a[i] >> 24; } }

static uint32_t sub8(fe r, const fe a, const fe b)            /* r = a - b, returns the borrow */
{
    uint64_t c = 0;
    for (int i = 0; i < 8; i++) { c = (uint64_t)a[i] - b[i] - (uint32_t)(c >> 63); r[i] = c; }
    return c >> 63;
}
static uint32_t add8(fe r, const fe a, const fe b)            /* returns the carry */
{
    uint64_t c = 0;
    for (int i = 0; i < 8; i++) { c += (uint64_t)a[i] + b[i]; r[i] = c; c >>= 32; }
    return c;
}
static void sel(fe r, const fe a, uint32_t mask) { for (int i = 0; i < 8; i++) r[i] = (r[i] & ~mask) | (a[i] & mask); }   /* mask ? a : r */
static int lt(const fe a, const fe m) { fe t; return sub8(t, a, m); }
static uint32_t is_zero(const fe a) { uint32_t o = 0; for (int i = 0; i < 8; i++) o |= a[i]; return -(uint32_t)(o == 0); }

static void madd(const mod_t *M, fe r, const fe a, const fe b)
{
    fe d; uint32_t c = add8(r, a, b), bo = sub8(d, r, M->m);
    sel(r, d, -(c | (bo ^ 1)));
}
static void msub(const mod_t *M, fe r, const fe a, const fe b)
{
    fe d; uint32_t bo = sub8(r, a, b);
    add8(d, r, M->m); sel(r, d, -bo);
}
static void mmul(const mod_t *M, fe r, const fe a, const fe b)   /* r = a b R^-1 mod m (CIOS) */
{
    uint32_t t[10] = { 0 };
    const uint32_t *m = M->m;
    for (int i = 0; i < 8; i++) {
        uint64_t c = 0;
        for (int j = 0; j < 8; j++) { c += (uint64_t)a[j] * b[i] + t[j]; t[j] = c; c >>= 32; }
        c += t[8]; t[8] = c; t[9] = c >> 32;
        uint32_t q = t[0] * M->mp;
        c = ((uint64_t)q * m[0] + t[0]) >> 32;
        for (int j = 1; j < 8; j++) { c += (uint64_t)q * m[j] + t[j]; t[j - 1] = c; c >>= 32; }
        c += t[8]; t[7] = c; t[8] = t[9] + (uint32_t)(c >> 32);
    }
    fe d; uint32_t bo = sub8(d, t, m);
    memcpy(r, t, 32); sel(r, d, -(t[8] | (bo ^ 1)));
}
static void mpow(const mod_t *M, fe r, const fe a, const fe e)   /* Montgomery-form a^e; e public (p-2, n-2) */
{
    fe x; memcpy(x, M->one, 32);
    for (int i = 255; i >= 0; i--) { mmul(M, x, x, x); if (e[i >> 5] >> (i & 31) & 1) mmul(M, x, x, a); }
    memcpy(r, x, 32);
}
static void minv(const mod_t *M, fe r, const fe a) { fe e, two = { 2 }; sub8(e, M->m, two); mpow(M, r, a, e); }
static void to_m(const mod_t *M, fe r, const fe a) { mmul(M, r, a, M->rr); }
static void from_m(const mod_t *M, fe r, const fe a) { fe one = { 1 }; mmul(M, r, a, one); }

static void mod_setup(mod_t *M, const uint32_t *m)
{
    M->m = m;
    uint32_t inv = 1; for (int i = 0; i < 5; i++) inv *= 2 - m[0] * inv;
    M->mp = -inv;
    fe z = { 0 }, t; sub8(M->one, z, m);                       /* R mod m = 2^256 - m, less m once more if m < 2^255 */
    if (!sub8(t, M->one, m)) memcpy(M->one, t, 32);
    memcpy(M->rr, M->one, 32);
    for (int i = 0; i < 256; i++) madd(M, M->rr, M->rr, M->rr);
}
static void init(void)
{
    if (ready) return;
    mod_setup(&mp_, P); mod_setup(&mn_, N);
    fe t; from_be(t, B_BE); to_m(&mp_, b_m, t);
    from_be(t, GX_BE); to_m(&mp_, G.x, t); from_be(t, GY_BE); to_m(&mp_, G.y, t); memcpy(G.z, mp_.one, 32);
    ready = 1;
}

#define FA(r, a, b) madd(&mp_, r, a, b)
#define FS(r, a, b) msub(&mp_, r, a, b)
#define FM(r, a, b) mmul(&mp_, r, a, b)

static void pdbl(pt *r, const pt *p)                           /* dbl-2001-b, a = -3; infinity stays infinity */
{
    fe d, g, be, al, t, u;
    FM(d, p->z, p->z); FM(g, p->y, p->y); FM(be, p->x, g);
    FS(t, p->x, d); FA(u, p->x, d); FM(al, t, u); FA(t, al, al); FA(al, t, al);
    FA(t, p->y, p->z); FM(t, t, t); FS(t, t, g); FS(r->z, t, d);
    FA(u, be, be); FA(u, u, u);                                /* 4 beta */
    FM(t, al, al); FS(t, t, u); FS(r->x, t, u);                /* X3 = alpha^2 - 8 beta */
    FS(u, u, r->x); FM(t, g, g); FA(t, t, t); FA(t, t, t); FA(t, t, t);   /* 8 gamma^2 */
    FM(u, al, u); FS(r->y, u, t);
}
static void padd(pt *r, const pt *p, const pt *q)              /* add-2007-bl; p, q != infinity, p != +-q (caller) */
{
    fe z1, z2, u1, u2, s1, s2, h, i, j, rr, v, t;
    FM(z1, p->z, p->z); FM(z2, q->z, q->z);
    FM(u1, p->x, z2); FM(u2, q->x, z1);
    FM(s1, p->y, q->z); FM(s1, s1, z2); FM(s2, q->y, p->z); FM(s2, s2, z1);
    FS(h, u2, u1); FA(i, h, h); FM(i, i, i); FM(j, h, i);
    FS(rr, s2, s1); FA(rr, rr, rr); FM(v, u1, i);
    FM(t, rr, rr); FS(t, t, j); FS(t, t, v); FS(t, t, v);
    fe y; FS(y, v, t); FM(y, rr, y); FM(s1, s1, j); FA(s1, s1, s1); FS(r->y, y, s1);
    memcpy(r->x, t, 32);
    FA(t, p->z, q->z); FM(t, t, t); FS(t, t, z1); FS(t, t, z2); FM(r->z, t, h);
}
static void pt_sel(pt *r, const pt *a, uint32_t mask) { sel(r->x, a->x, mask); sel(r->y, a->y, mask); sel(r->z, a->z, mask); }

/* r = k p, k in [1, n-1]: 4-bit fixed window, every table entry read, every step adds. For such k the running sum is
 * never +-T[d] (prefix*16 + d <= k < n), so only infinity needs a masked fix-up, no branches on secret data. */
static void pmul(pt *r, const fe k, const pt *p)
{
    static pt T[16];
    memset(&T[0], 0, sizeof T[0]); T[1] = *p; pdbl(&T[2], p);
    for (int i = 3; i < 16; i++) padd(&T[i], &T[i - 1], p);
    pt R; memset(&R, 0, sizeof R);
    for (int w = 63; w >= 0; w--) {
        for (int i = 0; i < 4; i++) pdbl(&R, &R);
        uint32_t d = k[w >> 3] >> ((w & 7) * 4) & 15;
        pt q; memset(&q, 0, sizeof q);
        for (uint32_t i = 1; i < 16; i++) pt_sel(&q, &T[i], -(uint32_t)(i == d));
        pt s; padd(&s, &R, &q);
        uint32_t rinf = is_zero(R.z), qinf = -(uint32_t)(d == 0);
        pt_sel(&s, &q, rinf); pt_sel(&s, &R, qinf);
        R = s;
    }
    *r = R;
}
static int affine(fe x, fe y, const pt *p)                     /* plain (non-Montgomery) affine coordinates */
{
    if (is_zero(p->z)) return -1;
    fe zi, z2; minv(&mp_, zi, p->z); FM(z2, zi, zi);
    FM(x, p->x, z2); from_m(&mp_, x, x);
    if (y) { FM(z2, z2, zi); FM(y, p->y, z2); from_m(&mp_, y, y); }
    return 0;
}
static int scalar_ok(const fe k) { return !is_zero(k) && lt(k, N); }

static int load_pub(pt *q, const uint8_t pub[64])              /* range + curve check: y^2 = x^3 - 3x + b */
{
    fe x, y; from_be(x, pub); from_be(y, pub + 32);
    if (!lt(x, P) || !lt(y, P)) return -1;
    to_m(&mp_, q->x, x); to_m(&mp_, q->y, y); memcpy(q->z, mp_.one, 32);
    fe l, r, t;
    FM(l, q->y, q->y);
    FM(r, q->x, q->x); FM(r, r, q->x); FA(t, q->x, q->x); FA(t, t, q->x); FS(r, r, t); FA(r, r, b_m);
    return memcmp(l, r, 32) ? -1 : 0;
}

/* P-256 operations share static tables (pmul's T[16], the Montgomery contexts): one caller at a time, both cores.
 * Not from interrupts (an operation takes milliseconds). Secrets (scalars, nonces, intermediates) are wiped on
 * every return path. */
static volatile uint32_t ecc_lk;
static void ecc_lock(void) { while (__atomic_exchange_n(&ecc_lk, 1, __ATOMIC_ACQUIRE)) {} }
static void ecc_unlock(void) { __atomic_store_n(&ecc_lk, 0, __ATOMIC_RELEASE); }
#define WIPE(...) do { void *w_[] = { __VA_ARGS__ }; for (unsigned i_ = 0; i_ < sizeof w_ / sizeof w_[0]; i_++) explicit_bzero(w_[i_], 32); } while (0)

int p256_check_pub(const uint8_t pub[64]) { ecc_lock(); init(); pt q; int r = load_pub(&q, pub); ecc_unlock(); return r; }

static int pubkey_nolock(uint8_t pub[64], const uint8_t priv[32])
{
    fe d, x, y; pt q; int r = -1;
    from_be(d, priv);
    if (scalar_ok(d)) { pmul(&q, d, &G); if (!affine(x, y, &q)) { to_be(pub, x); to_be(pub + 32, y); r = 0; } }
    WIPE(d); explicit_bzero(&q, sizeof q);
    return r;
}

int p256_pubkey(uint8_t pub[64], const uint8_t priv[32])
{
    ecc_lock(); init();
    int r = pubkey_nolock(pub, priv);
    ecc_unlock();
    return r;
}

int p256_keygen(uint8_t priv[32], uint8_t pub[64])
{
    fe d;
    do { rng_fill(priv, 32); from_be(d, priv); } while (!scalar_ok(d));
    WIPE(d);
    return p256_pubkey(pub, priv);
}

int p256_ecdh(uint8_t shared[32], const uint8_t priv[32], const uint8_t peer[64])
{
    ecc_lock(); init();
    fe d, x; pt q; int r = -1; from_be(d, priv);
    if (scalar_ok(d) && !load_pub(&q, peer)) { pmul(&q, d, &q); if (!affine(x, 0, &q)) { to_be(shared, x); r = 0; } }
    WIPE(d, x); explicit_bzero(&q, sizeof q);
    ecc_unlock();
    return r;
}

static void hash_mod_n(fe e, const uint8_t hash[32]) { from_be(e, hash); fe t; if (!sub8(t, e, N)) memcpy(e, t, 32); }

static int sign_k_nolock(uint8_t sig[64], const uint8_t priv[32], const uint8_t hash[32], const uint8_t kb[32])
{
    fe d, k, e, x, r, s, t; pt q; int rc = -1;
    from_be(d, priv); from_be(k, kb);
    if (!scalar_ok(d) || !scalar_ok(k)) goto out;
    pmul(&q, k, &G);
    if (affine(x, 0, &q)) goto out;
    if (sub8(r, x, N)) memcpy(r, x, 32);                       /* r = x mod n (x < p < 2n) */
    if (is_zero(r)) goto out;
    hash_mod_n(e, hash);
    to_m(&mn_, k, k); minv(&mn_, k, k);                        /* k^-1, Montgomery */
    to_m(&mn_, t, r); to_m(&mn_, d, d); mmul(&mn_, t, t, d);   /* r d */
    to_m(&mn_, e, e); madd(&mn_, t, t, e);                     /* e + r d */
    mmul(&mn_, s, t, k); from_m(&mn_, s, s);
    if (is_zero(s)) goto out;
    to_be(sig, r); to_be(sig + 32, s);
    rc = 0;
out:
    WIPE(d, k, e, x, t, s); explicit_bzero(&q, sizeof q);
    return rc;
}

static int verify_nolock(const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64]);

/* a fault during signing (glitch) can leak the key through a wrong signature: check it before it leaves */
static int sign_checked(uint8_t sig[64], const uint8_t priv[32], const uint8_t hash[32], const uint8_t kb[32])
{
    int rc = sign_k_nolock(sig, priv, hash, kb);
    if (!rc && NDF_ECC_VERIFY_SIGN) {
        uint8_t pub[64];
        if (pubkey_nolock(pub, priv) || verify_nolock(pub, hash, sig)) { explicit_bzero(sig, 64); rc = -1; }
    }
    return rc;
}

int p256_sign_k(uint8_t sig[64], const uint8_t priv[32], const uint8_t hash[32], const uint8_t kb[32])
{
    ecc_lock(); init();
    int rc = sign_checked(sig, priv, hash, kb);
    ecc_unlock();
    return rc;
}

int p256_sign(uint8_t sig[64], const uint8_t priv[32], const uint8_t hash[32])
{
    uint8_t kb[32], rnd[32]; fe k; int rc, tries = 0;
    ecc_lock(); init();
    from_be(k, priv);
    if (!scalar_ok(k)) { ecc_unlock(); return -1; }
    do {                                                       /* retries only on r = 0 or s = 0 */
        /* hedged nonce: SHA-256(random || key || hash) - a weak or repeating RNG can never repeat k for a new hash */
        do {
            rng_fill(rnd, 32);
            sha_ctx_t c; sha_start(&c, SHA256); sha_update(&c, rnd, 32); sha_update(&c, priv, 32); sha_update(&c, hash, 32);
            sha_finish(&c, kb); from_be(k, kb);
        } while (!scalar_ok(k));
        rc = sign_checked(sig, priv, hash, kb);
    } while (rc && ++tries < 8);
    explicit_bzero(kb, 32); explicit_bzero(rnd, 32); WIPE(k);
    ecc_unlock();
    return rc;
}

static int verify_nolock(const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64])
{
    init();
    fe r, s, e, w, u1, u2, x; pt q, a, b;
    from_be(r, sig); from_be(s, sig + 32);
    if (!scalar_ok(r) || !scalar_ok(s) || load_pub(&q, pub)) return -1;
    hash_mod_n(e, hash);
    to_m(&mn_, w, s); minv(&mn_, w, w);
    to_m(&mn_, u1, e); mmul(&mn_, u1, u1, w); from_m(&mn_, u1, u1);
    to_m(&mn_, u2, r); mmul(&mn_, u2, u2, w); from_m(&mn_, u2, u2);
    /* public data: plain branches are fine here */
    int i1 = is_zero(u1) != 0;
    pmul(&b, u2, &q);
    if (!i1) {
        pmul(&a, u1, &G);
        fe ua, ub, sa, sb, z1, z2;                              /* a == +-b?  compare X Z^2 and Y Z^3 */
        FM(z1, a.z, a.z); FM(z2, b.z, b.z); FM(ua, a.x, z2); FM(ub, b.x, z1);
        if (memcmp(ua, ub, 32)) padd(&b, &a, &b);
        else {
            FM(sa, a.y, b.z); FM(sa, sa, z2); FM(sb, b.y, a.z); FM(sb, sb, z1);
            if (memcmp(sa, sb, 32)) return -1;                 /* a = -b: infinity */
            pdbl(&b, &a);
        }
    }
    if (affine(x, 0, &b)) return -1;
    fe t; if (!sub8(t, x, N)) memcpy(x, t, 32);
    return memcmp(x, r, 32) ? -1 : 0;
}

/* ---- X25519 ---- */
static const fe P25 = { 0xFFFFFFED, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x7FFFFFFF };
static mod_t m25;
static fe a24_m;

static void cswap(fe a, fe b, uint32_t mask) { for (int i = 0; i < 8; i++) { uint32_t t = (a[i] ^ b[i]) & mask; a[i] ^= t; b[i] ^= t; } }

int p256_verify(const uint8_t pub[64], const uint8_t hash[32], const uint8_t sig[64])
{
    ecc_lock(); init();
    int r = verify_nolock(pub, hash, sig);
    ecc_unlock();
    return r;
}

int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32])
{
    if (!m25.m) { mod_setup(&m25, P25); fe a = { 121665 }; to_m(&m25, a24_m, a); }
    const mod_t *M = &m25;
    uint8_t k[32]; memcpy(k, scalar, 32); k[0] &= 248; k[31] = (k[31] & 127) | 64;
    fe x1, x2, z2 = { 0 }, x3, z3, a, aa, b, bb, e, c, d, t;
    for (int i = 0; i < 8; i++) x1[i] = (uint32_t)u[4*i] | u[4*i+1] << 8 | u[4*i+2] << 16 | (uint32_t)u[4*i+3] << 24;
    x1[7] &= 0x7FFFFFFF;
    if (!sub8(t, x1, P25)) memcpy(x1, t, 32);                  /* non-canonical u: reduce */
    to_m(M, x1, x1); memcpy(x2, M->one, 32); memcpy(x3, x1, 32); memcpy(z3, M->one, 32);
    uint32_t swap = 0;
    for (int i = 254; i >= 0; i--) {
        uint32_t bit = k[i >> 3] >> (i & 7) & 1;
        swap ^= bit; cswap(x2, x3, -swap); cswap(z2, z3, -swap); swap = bit;
        madd(M, a, x2, z2); mmul(M, aa, a, a); msub(M, b, x2, z2); mmul(M, bb, b, b); msub(M, e, aa, bb);
        madd(M, c, x3, z3); msub(M, d, x3, z3); mmul(M, d, d, a); mmul(M, c, c, b);
        madd(M, t, d, c); mmul(M, x3, t, t);
        msub(M, t, d, c); mmul(M, t, t, t); mmul(M, z3, x1, t);
        mmul(M, x2, aa, bb);
        mmul(M, t, a24_m, e); madd(M, t, aa, t); mmul(M, z2, e, t);
    }
    cswap(x2, x3, -swap); cswap(z2, z3, -swap);
    minv(M, z2, z2); mmul(M, x2, x2, z2); from_m(M, x2, x2);
    for (int i = 0; i < 8; i++) { out[4*i] = x2[i]; out[4*i+1] = x2[i] >> 8; out[4*i+2] = x2[i] >> 16; out[4*i+3] = x2[i] >> 24; }
    explicit_bzero(k, 32);
    uint32_t o = 0; for (int i = 0; i < 8; i++) o |= x2[i];
    return o ? 0 : -1;                                          /* all-zero shared secret: low-order input */
}

int x25519_base(uint8_t pub[32], const uint8_t priv[32]) { static const uint8_t nine[32] = { 9 }; return x25519(pub, priv, nine); }
