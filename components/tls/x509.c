/* Minimal X.509 for TLS (see x509.h). A DER reader that checks every length against the end of its parent; single-byte tags only. */
#include <string.h>
#include "x509.h"
#include "p256.h"
#include "sha.h"

static int tlv(const uint8_t **p, const uint8_t *end, uint8_t *tag, const uint8_t **val, unsigned *len)
{
    const uint8_t *q = *p;
    if (end - q < 2) return -1;
    *tag = q[0]; if ((q[0] & 0x1F) == 0x1F) return -1;           /* multi-byte tags do not occur here */
    unsigned l = q[1]; q += 2;
    if (l & 0x80) {
        unsigned nb = l & 0x7F; if (!nb || nb > 2 || end - q < (long)nb) return -1;
        l = 0; while (nb--) l = l << 8 | *q++;
    }
    if ((long)l > end - q) return -1;
    *val = q; *len = l; *p = q + l;
    return 0;
}
static int expect(const uint8_t **p, const uint8_t *end, uint8_t want, const uint8_t **val, unsigned *len)
{
    uint8_t t; if (tlv(p, end, &t, val, len) || t != want) return -1; return 0;
}

static const uint8_t oid_ecdsa_sha256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02 }, oid_ec_pub[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 },
                     oid_p256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 }, oid_cn[] = { 0x55, 0x04, 0x03 }, oid_san[] = { 0x55, 0x1D, 0x11 }, oid_bc[] = { 0x55, 0x1D, 0x13 };
#define OIDEQ(v, l, o) ((l) == sizeof o && !memcmp(v, o, sizeof o))

int x509_ecdsa_der(const uint8_t *der, unsigned n, uint8_t rs[64])
{
    const uint8_t *p = der, *end = der + n, *v; unsigned l;
    if (expect(&p, end, 0x30, &v, &l) || p != end) return -1;
    const uint8_t *q = v, *qe = v + l;
    for (int k = 0; k < 2; k++) {
        const uint8_t *iv; unsigned il;
        if (expect(&q, qe, 0x02, &iv, &il) || !il || (iv[0] & 0x80)) return -1;      /* positive integers only */
        while (il > 1 && !iv[0]) { iv++; il--; }
        if (il > 32) return -1;
        memset(rs + 32 * k, 0, 32 - il); memcpy(rs + 32 * k + 32 - il, iv, il);
    }
    return q == qe ? 0 : -1;
}

unsigned x509_ecdsa_to_der(const uint8_t rs[64], uint8_t *out)
{
    uint8_t *o = out + 2;
    for (int k = 0; k < 2; k++) {
        const uint8_t *v = rs + 32 * k; unsigned l = 32;
        while (l > 1 && !*v) { v++; l--; }
        *o++ = 0x02; *o++ = (uint8_t)(l + ((v[0] & 0x80) ? 1 : 0)); if (v[0] & 0x80) *o++ = 0;
        memcpy(o, v, l); o += l;
    }
    out[0] = 0x30; out[1] = (uint8_t)(o - out - 2);
    return (unsigned)(o - out);
}

int x509_parse(const uint8_t *der, unsigned n, x509_t *c)
{
    memset(c, 0, sizeof *c);
    const uint8_t *p = der, *end = der + n, *v; unsigned l;
    if (expect(&p, end, 0x30, &v, &l) || p != end) return -1;                       /* Certificate */
    const uint8_t *cp = v, *ce = v + l;
    const uint8_t *tbs_start = cp, *tv; unsigned tl;
    if (expect(&cp, ce, 0x30, &tv, &tl)) return -1;                                  /* tbsCertificate */
    c->tbs = tbs_start; c->tbs_len = (unsigned)(cp - tbs_start);
    const uint8_t *av; unsigned al;
    if (expect(&cp, ce, 0x30, &av, &al)) return -1;                                  /* signatureAlgorithm */
    { const uint8_t *ap = av, *ov; unsigned ol; if (expect(&ap, av + al, 0x06, &ov, &ol) || !OIDEQ(ov, ol, oid_ecdsa_sha256)) return -1; }
    const uint8_t *sv; unsigned sl;
    if (expect(&cp, ce, 0x03, &sv, &sl) || cp != ce || sl < 1 || sv[0] != 0 || x509_ecdsa_der(sv + 1, sl - 1, c->sig)) return -1;

    const uint8_t *t = tv, *te = tv + tl; uint8_t tag;
    if (tlv(&t, te, &tag, &v, &l)) return -1;
    if (tag == 0xA0 && tlv(&t, te, &tag, &v, &l)) return -1;                         /* optional version, then the serial number */
    if (tag != 0x02) return -1;
    if (expect(&t, te, 0x30, &av, &al)) return -1;                                   /* signature algorithm again */
    { const uint8_t *ap = av, *ov; unsigned ol; if (expect(&ap, av + al, 0x06, &ov, &ol) || !OIDEQ(ov, ol, oid_ecdsa_sha256)) return -1; }
    if (expect(&t, te, 0x30, &v, &l)) return -1;                                     /* issuer */
    if (expect(&t, te, 0x30, &v, &l)) return -1;                                     /* validity (not checked: no RTC) */
    if (expect(&t, te, 0x30, &v, &l)) return -1;                                     /* subject */
    for (const uint8_t *r = v, *re = v + l; r < re; ) {                              /* RDNs: SET { SEQUENCE { OID, string } } */
        const uint8_t *sv2; unsigned sl2;
        if (expect(&r, re, 0x31, &sv2, &sl2)) return -1;
        const uint8_t *a = sv2, *ae = sv2 + sl2, *atv; unsigned atl;
        if (expect(&a, ae, 0x30, &atv, &atl)) return -1;
        const uint8_t *o = atv, *oe = atv + atl, *ov, *sv3; unsigned ol, sl3; uint8_t st;
        if (expect(&o, oe, 0x06, &ov, &ol) || tlv(&o, oe, &st, &sv3, &sl3)) return -1;
        if (OIDEQ(ov, ol, oid_cn) && !c->cn) { c->cn = sv3; c->cn_len = sl3; }
    }
    if (expect(&t, te, 0x30, &v, &l)) return -1;                                     /* subjectPublicKeyInfo */
    { const uint8_t *k = v, *ke = v + l, *alg, *bv; unsigned algl, bl;
      if (expect(&k, ke, 0x30, &alg, &algl)) return -1;
      const uint8_t *ap = alg, *ae = alg + algl, *ov; unsigned ol;
      if (expect(&ap, ae, 0x06, &ov, &ol) || !OIDEQ(ov, ol, oid_ec_pub) || expect(&ap, ae, 0x06, &ov, &ol) || !OIDEQ(ov, ol, oid_p256)) return -1;
      if (expect(&k, ke, 0x03, &bv, &bl) || k != ke || bl != 66 || bv[0] != 0 || bv[1] != 4) return -1;   /* uncompressed point */
      memcpy(c->pub, bv + 2, 64);
      if (p256_check_pub(c->pub)) return -1; }
    while (t < te) {                                                                 /* issuer / subject unique ids, then the [3] extensions */
        if (tlv(&t, te, &tag, &v, &l)) return -1;
        if (tag != 0xA3) continue;
        const uint8_t *x = v, *xe = v + l, *ev; unsigned el;
        if (expect(&x, xe, 0x30, &ev, &el) || x != xe) return -1;
        for (const uint8_t *e = ev, *ee = ev + el; e < ee; ) {
            const uint8_t *xv; unsigned xl;
            if (expect(&e, ee, 0x30, &xv, &xl)) return -1;
            const uint8_t *f = xv, *fe = xv + xl, *ov, *vv; unsigned ol, vl; uint8_t ft;
            if (expect(&f, fe, 0x06, &ov, &ol) || tlv(&f, fe, &ft, &vv, &vl)) return -1;
            if (ft == 0x01 && tlv(&f, fe, &ft, &vv, &vl)) return -1;               /* critical flag, then the value */
            if (ft != 0x04) return -1;
            if (OIDEQ(ov, ol, oid_san)) { const uint8_t *s = vv, *sv4; unsigned sl4; if (expect(&s, vv + vl, 0x30, &sv4, &sl4)) return -1; c->san = sv4; c->san_len = sl4; }
            else if (OIDEQ(ov, ol, oid_bc)) { const uint8_t *s = vv, *sv4; unsigned sl4; if (expect(&s, vv + vl, 0x30, &sv4, &sl4)) return -1; if (sl4 >= 3 && sv4[0] == 0x01 && sv4[1] == 1 && sv4[2]) c->is_ca = 1; }
        }
    }
    return 0;
}

int x509_verify(const x509_t *c, const uint8_t ca_pub[64])
{
    uint8_t h[32]; sha(SHA256, c->tbs, c->tbs_len, h);
    return p256_verify(ca_pub, h, c->sig);
}

static int name_eq(const uint8_t *pat, unsigned pl, const char *host)
{
    unsigned hl = (unsigned)strlen(host); if (!pl || !hl) return -1;
    if (pl > 2 && pat[0] == '*' && pat[1] == '.') {                                  /* one wildcard label, never matching an empty one */
        const char *dot = host; while (*dot && *dot != '.') dot++; if (!*dot || dot == host) return -1;
        host = dot + 1; hl = (unsigned)strlen(host); pat += 2; pl -= 2;
    }
    if (pl != hl) return -1;
    for (unsigned i = 0; i < pl; i++) { char a = (char)pat[i], b = host[i]; if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; if (a != b) return -1; }
    return 0;
}

int x509_host_ok(const x509_t *c, const char *host)
{
    if (c->san) {
        int any = 0;
        for (const uint8_t *s = c->san, *se = c->san + c->san_len; s < se; ) {
            uint8_t tag; const uint8_t *v; unsigned l; if (tlv(&s, se, &tag, &v, &l)) return -1;
            if (tag == 0x82) { any = 1; if (!name_eq(v, l, host)) return 0; }
        }
        if (any) return -1;                                                           /* a SAN with dNSNames replaces the CN */
    }
    return c->cn && !name_eq(c->cn, c->cn_len, host) ? 0 : -1;
}
