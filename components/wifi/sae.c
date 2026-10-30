/* WPA3-SAE, group 19, as IDF's wpa_supplicant (src/common/sae.c, dragonfly.c): hunting-and-pecking PWE with at least 40 rounds, commit
 * scalar = (rand + mask) mod r, element = -(mask PWE), K = rand (peer_scalar PWE + peer_element), keyseed = HMAC-SHA256(0^32, k),
 * KCK | PMK = KDF-512(keyseed, "SAE KCK and PMK", (scalar + peer_scalar) mod r), confirm = HMAC-SHA256(KCK, sc | s | E | s' | E'). */
#include <string.h>
#include "sae.h"
#include "wpa.h"
#include "p256.h"
#include "rng.h"

static const uint8_t PRIME[32] = { 0xff,0xff,0xff,0xff,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
static void wipe(void *p, unsigned n) { volatile uint8_t *q = p; while (n--) *q++ = 0; }
static uint8_t lt_be(const uint8_t *a, const uint8_t *b) { int r = 0, d = 0; for (int i = 0; i < 32; i++) { int x = (a[i] > b[i]) - (a[i] < b[i]); r = d ? r : x; d |= x; } return r < 0; }

static int derive_pwe(sae_t *s, const uint8_t me[6], const uint8_t peer[6], const char *pw, unsigned pl)
{
    uint8_t addrs[12], seed[32], x[32], cand[32], y[32], found = 0, odd = 0, counter;
    int hi = memcmp(me, peer, 6) > 0;
    memcpy(addrs, hi ? me : peer, 6); memcpy(addrs + 6, hi ? peer : me, 6);       /* max || min */
    memset(x, 0, sizeof x);
    for (counter = 1; counter <= 40 || !found; counter++) {
        if (counter > 200) return -1;
        const uint8_t *p[2] = { (const uint8_t *)pw, &counter }; unsigned l[2] = { pl, 1 };
        wpa_hmac_sha256_v(addrs, 12, 2, p, l, seed);
        wpa_kdf_sha256(seed, 32, "SAE Hunting and Pecking", PRIME, 32, cand, 256);
        uint8_t ok = lt_be(cand, PRIME) && !p256_sae_y(cand, y);                   /* every round runs the square test (p256_sae_y's cost does not depend on the answer) */
        uint8_t take = ok & !found;
        for (int i = 0; i < 32; i++) x[i] = take ? cand[i] : x[i];
        odd = take ? seed[31] & 1 : odd;
        found |= ok;
    }
    if (p256_sae_y(x, y)) return -1;
    memcpy(s->pwe, x, 32); memcpy(s->pwe + 32, y, 32);
    if ((y[31] & 1) != odd) p256_point_neg(s->pwe, s->pwe);                    /* y with the parity of the seed, else p - y */
    wipe(seed, sizeof seed); wipe(cand, sizeof cand); wipe(y, sizeof y); wipe(x, sizeof x);
    return 0;
}

static int commit(sae_t *s, const uint8_t mask[32])
{
    uint8_t me[64];
    if (p256_scalar_add(s->scalar, s->rand, mask) || !p256_scalar_ok(s->scalar)) return -1;
    if (p256_point_mul(me, mask, s->pwe)) return -1;
    p256_point_neg(s->elem, me); wipe(me, sizeof me);
    s->send_confirm = 0;
    return 0;
}

int sae_prepare_fixed(sae_t *s, const uint8_t me[6], const uint8_t peer[6], const char *pw, unsigned pl, const uint8_t rand[32], const uint8_t mask[32])
{
    if (derive_pwe(s, me, peer, pw, pl)) return -1;
    memcpy(s->rand, rand, 32);
    return commit(s, mask);
}

int sae_prepare(sae_t *s, const uint8_t me[6], const uint8_t peer[6], const char *pw, unsigned pl)
{
    if (derive_pwe(s, me, peer, pw, pl)) return -1;
    static const uint8_t one[32] = { [31] = 1 };
    for (int n = 0; n < 100; n++) {                                               /* 1 < rand, mask < r and the scalar > 1 (dragonfly_generate_scalar) */
        uint8_t mask[32];
        rng_fill(s->rand, 32); rng_fill(mask, 32);
        if (!p256_scalar_ok(s->rand) || !p256_scalar_ok(mask) || !memcmp(s->rand, one, 32) || !memcmp(mask, one, 32)) continue;
        int r = commit(s, mask); wipe(mask, sizeof mask);
        if (!r && memcmp(s->scalar, one, 32)) return 0;
    }
    return -1;
}

unsigned sae_write_commit(const sae_t *s, uint8_t *o, const uint8_t *token, unsigned tl)
{
    unsigned n = 0;
    o[n++] = 19; o[n++] = 0;
    if (token && tl) { memcpy(o + n, token, tl); n += tl; }
    memcpy(o + n, s->scalar, 32); n += 32; memcpy(o + n, s->elem, 64); n += 64;
    return n;
}

int sae_process_commit(sae_t *s, const uint8_t *b, unsigned n)
{
    if (n < 2 + 32 + 64 || b[0] != 19 || b[1] != 0) return -1;                 /* group 19 only */
    const uint8_t *ps = b + 2, *pe = b + 34;
    if (!p256_scalar_ok(ps) || p256_check_pub(pe)) return -1;                    /* scalar in (0, r), element on the curve */
    if (!memcmp(ps, s->scalar, 32) && !memcmp(pe, s->elem, 64)) return -1;      /* our own commit reflected back */
    memcpy(s->peer_scalar, ps, 32); memcpy(s->peer_elem, pe, 64);
    uint8_t t[64], k[64], keyseed[32], zero[32] = { 0 }, ctx[32], keys[64]; int r = -1;
    if (!p256_point_mul(t, s->peer_scalar, s->pwe) && !p256_point_add(t, t, s->peer_elem) && !p256_point_mul(k, s->rand, t)) {
        wpa_hmac_sha256(zero, 32, k, 32, keyseed);
        p256_scalar_add(ctx, s->scalar, s->peer_scalar);
        wpa_kdf_sha256(keyseed, 32, "SAE KCK and PMK", ctx, 32, keys, 512);
        memcpy(s->kck, keys, 32); memcpy(s->pmk, keys + 32, 32); memcpy(s->pmkid, ctx, 16);
        r = 0;
    }
    wipe(t, sizeof t); wipe(k, sizeof k); wipe(keyseed, sizeof keyseed); wipe(keys, sizeof keys); wipe(ctx, sizeof ctx);
    return r;
}

static void cn(const sae_t *s, const uint8_t sc[2], int own_first, uint8_t out[32])
{
    const uint8_t *p[5] = { sc, own_first ? s->scalar : s->peer_scalar, own_first ? s->elem : s->peer_elem, own_first ? s->peer_scalar : s->scalar, own_first ? s->peer_elem : s->elem };
    unsigned l[5] = { 2, 32, 64, 32, 64 };
    wpa_hmac_sha256_v(s->kck, 32, 5, p, l, out);
}

unsigned sae_write_confirm(sae_t *s, uint8_t *o)
{
    if (s->send_confirm < 0xffff) s->send_confirm++;
    o[0] = (uint8_t)s->send_confirm; o[1] = (uint8_t)(s->send_confirm >> 8);
    cn(s, o, 1, o + 2);
    return 34;
}

int sae_check_confirm(const sae_t *s, const uint8_t *b, unsigned n)
{
    if (n < 34) return -1;
    uint8_t v[32], d = 0; cn(s, b, 0, v);
    for (int i = 0; i < 32; i++) d |= v[i] ^ b[2 + i];
    wipe(v, sizeof v);
    return d ? -1 : 0;
}

void sae_wipe(sae_t *s) { wipe(s, sizeof *s); }
