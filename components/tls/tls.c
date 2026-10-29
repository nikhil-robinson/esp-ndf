/* TLS 1.3 engine (RFC 8446), sans-IO, one cipher suite. See tls.h and docs/superpowers/plans/2026-10-29-tls.md. */
#include <string.h>
#include "tls.h"
#include "aes.h"
#include "x25519.h"
#include "p256.h"
#include "rng.h"

enum { C_WAIT_SH, C_WAIT_EE, C_WAIT_CERT, C_WAIT_CV, C_WAIT_FIN, S_WAIT_CH, S_WAIT_FIN, ESTABLISHED };
enum { HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_TICKET = 4, HS_ENC_EXT = 8, HS_CERT = 11, HS_CERT_REQ = 13, HS_CERT_VERIFY = 15, HS_FINISHED = 20, HS_KEY_UPDATE = 24 };
enum { ALERT_UNEXPECTED = 10, ALERT_BAD_MAC = 20, ALERT_OVERFLOW = 22, ALERT_HANDSHAKE_FAIL = 40, ALERT_BAD_CERT = 42, ALERT_ILLEGAL_PARAM = 47, ALERT_DECODE = 50,
       ALERT_DECRYPT = 51, ALERT_PROTO_VERSION = 70, ALERT_INTERNAL = 80 };

static const uint8_t *fix_rnd, *fix_priv, *fix_sess;
void tls_test_fix_random(const uint8_t rnd[32], const uint8_t priv[32], const uint8_t sess[32]) { fix_rnd = rnd; fix_priv = priv; fix_sess = sess; }
static void rand_fill(uint8_t *p, unsigned n, const uint8_t *fixed) { if (fixed) memcpy(p, fixed, n); else rng_fill(p, n); }

static void wipe(void *p, unsigned n) { volatile uint8_t *q = p; while (n--) *q++ = 0; }
static unsigned be16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }
static unsigned be24(const uint8_t *p) { return (unsigned)(p[0] << 16 | p[1] << 8 | p[2]); }
static int ct_eq(const uint8_t *a, const uint8_t *b, unsigned n) { uint8_t d = 0; while (n--) d |= *a++ ^ *b++; return d == 0; }

/* --- transcript --- */
static void th_add(tls_t *t, const uint8_t *m, unsigned n) { sha_update(&t->th, m, n); }
static void th_get(tls_t *t, uint8_t out[32]) { sha_ctx_t c = t->th; sha_invalidate(); sha_finish(&c, out); sha_invalidate(); }

/* --- records --- */
static void set_keys(const uint8_t sec[32], uint8_t key[16], uint8_t iv[12]) { tls_expand_label(sec, "key", 0, 0, key, 16); tls_expand_label(sec, "iv", 0, 0, iv, 12); }
static void nonce_of(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12]) { memcpy(nonce, iv, 12); for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(seq >> (8 * i)); }
static void use_write(tls_t *t, const uint8_t sec[32]) { set_keys(sec, t->wkey, t->wiv); t->wseq = 0; t->wenc = 1; }
static void use_read(tls_t *t, const uint8_t sec[32]) { set_keys(sec, t->rkey, t->riv); t->rseq = 0; t->renc = 1; }

/* the plaintext of n bytes is at o + 5; writes the record around it (protected once write keys exist); returns the record length */
static unsigned seal(tls_t *t, uint8_t type, uint8_t *o, unsigned n)
{
    if (!t->wenc) { o[0] = type; o[1] = 3; o[2] = (type == 22 && !t->is_server && o == t->wbuf) ? 1 : 3; o[3] = (uint8_t)(n >> 8); o[4] = (uint8_t)n; return 5 + n; }
    unsigned cl = n + 1 + 16; uint8_t nonce[12];
    o[0] = 23; o[1] = 3; o[2] = 3; o[3] = (uint8_t)(cl >> 8); o[4] = (uint8_t)cl; o[5 + n] = type;
    nonce_of(t->wiv, t->wseq++, nonce);
    aes_gcm(t->wkey, 16, true, nonce, 12, o, 5, o + 5, o + 5, n + 1, o + 5 + n + 1);
    return 5 + cl;
}
static int put_record(tls_t *t, uint8_t type, const uint8_t *body, unsigned n)
{
    if (t->wlen + 5 + n + 17 > sizeof t->wbuf) return -1;
    uint8_t *o = t->wbuf + t->wlen; memcpy(o + 5, body, n);
    t->wlen = (uint16_t)(t->wlen + seal(t, type, o, n));
    return 0;
}
static void fail(tls_t *t, int err, uint8_t alert)
{
    if (t->state == TLS_FAILED) return;
    uint8_t a[2] = { 2, alert }; put_record(t, 21, a, 2);
    t->state = TLS_FAILED; t->err = (int8_t)err; wipe(t->priv, 32); t->rplain_len = 0;
}

/* --- key schedule (RFC 8446 7.1) --- */
static void derive_hs(tls_t *t, const uint8_t shared[32])
{
    static const uint8_t zero[32] = { 0 };
    uint8_t early[32], empty[32], derived[32], hs[32], h[32];
    tls_hkdf_extract(0, 0, zero, 32, early);
    sha(SHA256, "", 0, empty);
    tls_derive_secret(early, "derived", empty, derived);
    tls_hkdf_extract(derived, 32, shared, 32, hs);
    th_get(t, h);
    tls_derive_secret(hs, "c hs traffic", h, t->sec_hs_c); tls_derive_secret(hs, "s hs traffic", h, t->sec_hs_s);
    tls_derive_secret(hs, "derived", empty, derived); tls_hkdf_extract(derived, 32, zero, 32, t->master);
    wipe(early, 32); wipe(derived, 32); wipe(hs, 32);
}
static void derive_ap(tls_t *t, const uint8_t h[32]) { tls_derive_secret(t->master, "c ap traffic", h, t->sec_ap_c); tls_derive_secret(t->master, "s ap traffic", h, t->sec_ap_s); wipe(t->master, 32); }
static void finished_mac(const uint8_t base[32], const uint8_t h[32], uint8_t out[32]) { uint8_t fk[32]; tls_expand_label(base, "finished", 0, 0, fk, 32); tls_hmac(fk, 32, h, 32, out); wipe(fk, 32); }
static void cv_input(const uint8_t h[32], int server, uint8_t out[98 + 32])
{
    memset(out, 0x20, 64); memcpy(out + 64, server ? "TLS 1.3, server CertificateVerify" : "TLS 1.3, client CertificateVerify", 33); out[97] = 0; memcpy(out + 98, h, 32);
}
static unsigned hs_hdr(uint8_t *o, uint8_t type, unsigned len) { o[0] = type; o[1] = (uint8_t)(len >> 16); o[2] = (uint8_t)(len >> 8); o[3] = (uint8_t)len; return 4; }

/* --- client --- */
static void build_client_hello(tls_t *t)
{
    uint8_t m[256]; unsigned n = 4; uint8_t pub[32], rnd[32];
    rand_fill(rnd, 32, fix_rnd); rand_fill(t->sess, 32, fix_sess); t->sess_len = 32; rand_fill(t->priv, 32, fix_priv); x25519_base(pub, t->priv);
    m[n++] = 3; m[n++] = 3; memcpy(m + n, rnd, 32); n += 32; m[n++] = 32; memcpy(m + n, t->sess, 32); n += 32;
    m[n++] = 0; m[n++] = 2; m[n++] = 0x13; m[n++] = 0x01; m[n++] = 1; m[n++] = 0;
    unsigned ext = n; n += 2;
    static const uint8_t fixed[] = { 0x00, 0x2b, 0x00, 0x03, 0x02, 0x03, 0x04,            /* supported_versions: TLS 1.3 */
                                     0x00, 0x0a, 0x00, 0x04, 0x00, 0x02, 0x00, 0x1d,        /* supported_groups: x25519 */
                                     0x00, 0x0d, 0x00, 0x04, 0x00, 0x02, 0x04, 0x03,        /* signature_algorithms: ecdsa_secp256r1_sha256 */
                                     0x00, 0x01, 0x00, 0x01, 0x04 };                        /* max_fragment_length: 2^12 */
    memcpy(m + n, fixed, sizeof fixed); n += sizeof fixed;
    static const uint8_t ks[] = { 0x00, 0x33, 0x00, 0x26, 0x00, 0x24, 0x00, 0x1d, 0x00, 0x20 };
    memcpy(m + n, ks, sizeof ks); n += sizeof ks; memcpy(m + n, pub, 32); n += 32;
    const char *h = t->ccfg->host; unsigned hl = h ? (unsigned)strlen(h) : 0;
    if (hl && hl < 100) {                                                              /* server_name */
        m[n++] = 0; m[n++] = 0; m[n++] = (uint8_t)((hl + 5) >> 8); m[n++] = (uint8_t)(hl + 5); m[n++] = (uint8_t)((hl + 3) >> 8); m[n++] = (uint8_t)(hl + 3); m[n++] = 0;
        m[n++] = (uint8_t)(hl >> 8); m[n++] = (uint8_t)hl; memcpy(m + n, h, hl); n += hl;
    }
    m[ext] = (uint8_t)((n - ext - 2) >> 8); m[ext + 1] = (uint8_t)(n - ext - 2);
    hs_hdr(m, HS_CLIENT_HELLO, n - 4);
    th_add(t, m, n); put_record(t, 22, m, n);
}

void tls_client_init(tls_t *t, const tls_client_cfg_t *c)
{
    memset(t, 0, sizeof *t); t->ccfg = c; t->hs = C_WAIT_SH; sha_start(&t->th, SHA256);
    if (!c || (!c->ca_pub && !c->pin_leaf)) { t->state = TLS_FAILED; t->err = TLS_ERR_CERT; return; }       /* no trust anchor: never connect */
    build_client_hello(t);
}

static int server_hello_rx(tls_t *t, const uint8_t *m, unsigned n)              /* m = body */
{
    static const uint8_t hrr[32] = { 0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };
    if (n < 2 + 32 + 1) return TLS_ERR_PROTO;
    if (!memcmp(m + 2, hrr, 32)) return TLS_ERR_UNSUPPORTED;                          /* HelloRetryRequest */
    const uint8_t *p = m + 34, *e = m + n; unsigned sl = *p++;
    if (e - p < (long)sl + 5 || sl != t->sess_len || memcmp(p, t->sess, sl)) return TLS_ERR_PROTO;
    p += sl;
    if (p[0] != 0x13 || p[1] != 0x01 || p[2] != 0) return TLS_ERR_UNSUPPORTED;        /* cipher suite, compression */
    p += 3; unsigned xl = be16(p); p += 2; if (e - p != (long)xl) return TLS_ERR_PROTO;
    int v13 = 0; const uint8_t *key = 0;
    while (p < e) {
        if (e - p < 4) return TLS_ERR_PROTO;
        unsigned ty = be16(p), l = be16(p + 2); p += 4; if (e - p < (long)l) return TLS_ERR_PROTO;
        if (ty == 0x002b) { if (l != 2 || p[0] != 3 || p[1] != 4) return TLS_ERR_UNSUPPORTED; v13 = 1; }
        else if (ty == 0x0033) { if (l != 36 || be16(p) != 0x001d || be16(p + 2) != 32) return TLS_ERR_UNSUPPORTED; key = p + 4; }
        p += l;
    }
    if (!v13) return TLS_ERR_UNSUPPORTED;
    if (!key) return TLS_ERR_PROTO;
    uint8_t shared[32];
    if (x25519(shared, t->priv, key)) return TLS_ERR_PROTO;                           /* low-order point */
    wipe(t->priv, 32);
    derive_hs(t, shared); wipe(shared, 32);
    use_read(t, t->sec_hs_s);
    return 0;
}

static int cert_rx(tls_t *t, const uint8_t *m, unsigned n)
{
    if (n < 4 || m[0] != 0) return TLS_ERR_PROTO;                                     /* request context must be empty */
    unsigned ll = be24(m + 1); if (ll + 4 != n || ll < 5) return TLS_ERR_PROTO;
    unsigned cl = be24(m + 4); if (cl + 3 + 2 > ll) return TLS_ERR_PROTO;
    x509_t c;
    if (x509_parse(m + 7, cl, &c)) return TLS_ERR_CERT;
    const tls_client_cfg_t *cfg = t->ccfg;
    if (cfg->pin_leaf) { if (!ct_eq(cfg->pin_leaf, c.pub, 64)) return TLS_ERR_CERT; }
    else if (x509_verify(&c, cfg->ca_pub)) return TLS_ERR_CERT;
    if (cfg->host && x509_host_ok(&c, cfg->host)) return TLS_ERR_NAME;
    memcpy(t->leaf, c.pub, 64);
    return 0;
}

static int cert_verify_rx(tls_t *t, const uint8_t *m, unsigned n, int server_signs)
{
    if (n < 4 || be16(m) != 0x0403 || be16(m + 2) != n - 4) return TLS_ERR_SIG;
    uint8_t rs[64], in[98 + 32], hh[32], h[32];
    if (x509_ecdsa_der(m + 4, n - 4, rs)) return TLS_ERR_SIG;
    th_get(t, hh); cv_input(hh, server_signs, in); sha(SHA256, in, sizeof in, h);
    return p256_verify(t->leaf, h, rs) ? TLS_ERR_SIG : 0;
}

/* --- server --- */
static int client_hello_rx(tls_t *t, const uint8_t *m, unsigned n)               /* m = body */
{
    if (n < 2 + 32 + 1) return TLS_ERR_PROTO;
    const uint8_t *p = m + 34, *e = m + n; unsigned sl = *p++;
    if (sl > 32 || e - p < (long)sl + 2) return TLS_ERR_PROTO;
    memcpy(t->sess, p, sl); t->sess_len = (uint8_t)sl; p += sl;
    unsigned csl = be16(p); p += 2; if ((csl & 1) || e - p < (long)csl + 1) return TLS_ERR_PROTO;
    int suite = 0; for (unsigned i = 0; i < csl; i += 2) if (p[i] == 0x13 && p[i + 1] == 0x01) suite = 1;
    p += csl; unsigned cm = *p++; if (e - p < (long)cm + 2) return TLS_ERR_PROTO; p += cm;
    unsigned xl = be16(p); p += 2; if (e - p != (long)xl) return TLS_ERR_PROTO;
    int v13 = 0, sig = 0; const uint8_t *ks = 0; t->mfl = 0;
    while (p < e) {
        if (e - p < 4) return TLS_ERR_PROTO;
        unsigned ty = be16(p), l = be16(p + 2); p += 4; if (e - p < (long)l) return TLS_ERR_PROTO;
        if (ty == 0x002b) { if (!l || l != 1u + p[0] || (p[0] & 1)) return TLS_ERR_PROTO; for (unsigned i = 1; i < l; i += 2) if (p[i] == 3 && p[i + 1] == 4) v13 = 1; }
        else if (ty == 0x000d) { if (l < 2 || be16(p) != l - 2) return TLS_ERR_PROTO; for (unsigned i = 2; i + 1 < l; i += 2) if (p[i] == 4 && p[i + 1] == 3) sig = 1; }
        else if (ty == 0x0033) {
            if (l < 2 || be16(p) != l - 2) return TLS_ERR_PROTO;
            for (const uint8_t *q = p + 2, *qe = p + l; q < qe; ) { if (qe - q < 4) return TLS_ERR_PROTO; unsigned g = be16(q), kl = be16(q + 2); q += 4; if (qe - q < (long)kl) return TLS_ERR_PROTO; if (g == 0x001d && kl == 32) ks = q; q += kl; }
        } else if (ty == 0x0001 && l == 1 && p[0] == 4) t->mfl = 1;
        p += l;
    }
    if (!v13 || !suite || !sig || !ks) return TLS_ERR_UNSUPPORTED;                    /* another version / group would need a HelloRetryRequest or a fallback */
    uint8_t shared[32], pub[32];
    rand_fill(t->priv, 32, fix_priv); x25519_base(pub, t->priv);
    if (x25519(shared, t->priv, ks)) return TLS_ERR_PROTO;
    wipe(t->priv, 32);

    /* ServerHello, in the clear */
    uint8_t sh[4 + 2 + 32 + 1 + 32 + 3 + 2 + 6 + 4 + 36]; unsigned k = 4;
    sh[k++] = 3; sh[k++] = 3; rand_fill(sh + k, 32, fix_rnd); k += 32; sh[k++] = (uint8_t)t->sess_len; memcpy(sh + k, t->sess, t->sess_len); k += t->sess_len;
    sh[k++] = 0x13; sh[k++] = 0x01; sh[k++] = 0;
    sh[k++] = 0; sh[k++] = 46;                                                       /* 6 + 40 bytes of extensions */
    static const uint8_t sv[] = { 0x00, 0x2b, 0x00, 0x02, 0x03, 0x04, 0x00, 0x33, 0x00, 0x24, 0x00, 0x1d, 0x00, 0x20 };
    memcpy(sh + k, sv, sizeof sv); k += sizeof sv; memcpy(sh + k, pub, 32); k += 32;
    hs_hdr(sh, HS_SERVER_HELLO, k - 4);
    th_add(t, sh, k);
    if (put_record(t, 22, sh, k)) return TLS_ERR_OVERFLOW;
    derive_hs(t, shared); wipe(shared, 32);

    /* EncryptedExtensions, Certificate, CertificateVerify, Finished in one protected record, built in place in the output queue */
    use_write(t, t->sec_hs_s); use_read(t, t->sec_hs_c);
    if (t->scfg->cert_len > 3000 || t->wlen + 5 + 4 + 11 + 4 + 4 + t->scfg->cert_len + 10 + 4 + 4 + 76 + 36 + 1 + 16 > sizeof t->wbuf) return TLS_ERR_OVERFLOW;     /* the whole flight must fit the output queue */
    uint8_t *o = t->wbuf + t->wlen, *b = o + 5; unsigned n2 = 0, mark;
    mark = n2; n2 += 4; if (t->mfl) { b[n2++] = 0; b[n2++] = 5; b[n2++] = 0; b[n2++] = 1; b[n2++] = 0; b[n2++] = 1; b[n2++] = 4; } else { b[n2++] = 0; b[n2++] = 0; }
    hs_hdr(b + mark, HS_ENC_EXT, n2 - mark - 4); th_add(t, b + mark, n2 - mark);
    const tls_server_cfg_t *cfg = t->scfg;
    mark = n2; n2 += 4; b[n2++] = 0; n2 += 3; n2 += 0;                                /* request context (empty), list length below */
    unsigned lstart = n2;
    b[n2++] = (uint8_t)(cfg->cert_len >> 16); b[n2++] = (uint8_t)(cfg->cert_len >> 8); b[n2++] = (uint8_t)cfg->cert_len; memcpy(b + n2, cfg->cert, cfg->cert_len); n2 += cfg->cert_len; b[n2++] = 0; b[n2++] = 0;
    b[mark + 5] = (uint8_t)((n2 - lstart) >> 16); b[mark + 6] = (uint8_t)((n2 - lstart) >> 8); b[mark + 7] = (uint8_t)(n2 - lstart);
    hs_hdr(b + mark, HS_CERT, n2 - mark - 4); th_add(t, b + mark, n2 - mark);
    { uint8_t hh[32], in[98 + 32], h[32], rs[64]; th_get(t, hh); cv_input(hh, 1, in); sha(SHA256, in, sizeof in, h);
      if (p256_sign(rs, cfg->priv, h)) return TLS_ERR_SIG;
      mark = n2; n2 += 4; b[n2++] = 0x04; b[n2++] = 0x03; uint8_t der[72]; unsigned dl = x509_ecdsa_to_der(rs, der); b[n2++] = 0; b[n2++] = (uint8_t)dl; memcpy(b + n2, der, dl); n2 += dl;
      hs_hdr(b + mark, HS_CERT_VERIFY, n2 - mark - 4); th_add(t, b + mark, n2 - mark); }
    { uint8_t hh[32], vd[32]; th_get(t, hh); finished_mac(t->sec_hs_s, hh, vd);
      mark = n2; n2 += 4; memcpy(b + n2, vd, 32); n2 += 32; hs_hdr(b + mark, HS_FINISHED, 32); th_add(t, b + mark, n2 - mark); }
    th_get(t, t->fin_hash);                                                          /* transcript through the server Finished */
    t->wlen = (uint16_t)(t->wlen + seal(t, 22, o, n2));
    derive_ap(t, t->fin_hash);
    use_write(t, t->sec_ap_s);                                                       /* our flight is sealed: from here on application keys */
    return 0;
}

/* --- handshake dispatch --- */
static int on_hs(tls_t *t, uint8_t type, const uint8_t *full, unsigned fl)           /* full = header + body */
{
    const uint8_t *b = full + 4; unsigned n = fl - 4; int r;
    switch (t->hs) {
    case C_WAIT_SH: if (type != HS_SERVER_HELLO) return TLS_ERR_PROTO; th_add(t, full, fl); if ((r = server_hello_rx(t, b, n))) return r; t->hs = C_WAIT_EE; return 0;
    case C_WAIT_EE: if (type == HS_CERT_REQ) return TLS_ERR_UNSUPPORTED; if (type != HS_ENC_EXT || n < 2 || be16(b) != n - 2) return TLS_ERR_PROTO; th_add(t, full, fl); t->hs = C_WAIT_CERT; return 0;
    case C_WAIT_CERT: if (type == HS_CERT_REQ) return TLS_ERR_UNSUPPORTED; if (type != HS_CERT) return TLS_ERR_PROTO; if ((r = cert_rx(t, b, n))) return r; th_add(t, full, fl); t->hs = C_WAIT_CV; return 0;
    case C_WAIT_CV: if (type != HS_CERT_VERIFY) return TLS_ERR_PROTO; if ((r = cert_verify_rx(t, b, n, 1))) return r; th_add(t, full, fl); t->hs = C_WAIT_FIN; return 0;
    case C_WAIT_FIN: {
        if (type != HS_FINISHED || n != 32) return TLS_ERR_PROTO;
        uint8_t hh[32], vd[32]; th_get(t, hh); finished_mac(t->sec_hs_s, hh, vd);
        if (!ct_eq(vd, b, 32)) return TLS_ERR_FINISHED;
        th_add(t, full, fl); th_get(t, t->fin_hash); derive_ap(t, t->fin_hash);
        finished_mac(t->sec_hs_c, t->fin_hash, vd);                                  /* client Finished, still under the handshake keys */
        uint8_t m[36]; hs_hdr(m, HS_FINISHED, 32); memcpy(m + 4, vd, 32);
        use_write(t, t->sec_hs_c); if (put_record(t, 22, m, 36)) return TLS_ERR_OVERFLOW;
        use_write(t, t->sec_ap_c); use_read(t, t->sec_ap_s);
        t->hs = ESTABLISHED; t->state = TLS_CONNECTED; return 0; }
    case S_WAIT_CH: if (type != HS_CLIENT_HELLO) return TLS_ERR_PROTO; th_add(t, full, fl); t->hs = S_WAIT_FIN; return client_hello_rx(t, b, n);
    case S_WAIT_FIN: {
        if (type != HS_FINISHED || n != 32) return TLS_ERR_PROTO;
        uint8_t vd[32]; finished_mac(t->sec_hs_c, t->fin_hash, vd);
        if (!ct_eq(vd, b, 32)) return TLS_ERR_FINISHED;
        use_read(t, t->sec_ap_c); t->hs = ESTABLISHED; t->state = TLS_CONNECTED; return 0; }
    default:                                                                         /* established: tickets are ignored, KeyUpdate honoured */
        if (type == HS_TICKET) return 0;
        if (type == HS_KEY_UPDATE && n == 1 && b[0] <= 1) {
            uint8_t *rsec = t->is_server ? t->sec_ap_c : t->sec_ap_s, *wsec = t->is_server ? t->sec_ap_s : t->sec_ap_c;
            tls_expand_label(rsec, "traffic upd", 0, 0, rsec, 32); use_read(t, rsec);
            if (b[0] == 1) { uint8_t m[5]; hs_hdr(m, HS_KEY_UPDATE, 1); m[4] = 0; if (put_record(t, 22, m, 5)) return TLS_ERR_OVERFLOW; tls_expand_label(wsec, "traffic upd", 0, 0, wsec, 32); use_write(t, wsec); }
            return 0;
        }
        return TLS_ERR_PROTO;
    }
}

static int handshake_rx(tls_t *t, const uint8_t *p, unsigned n)
{
    while (n) {
        if (n < 4) return TLS_ERR_PROTO;
        unsigned l = be24(p + 1); if (4 + l > n) return TLS_ERR_UNSUPPORTED;         /* a message split over records */
        int r = on_hs(t, p[0], p, 4 + l); if (r) return r;
        p += 4 + l; n -= 4 + l;
        if (t->state == TLS_FAILED) return t->err;
    }
    return 0;
}

static uint8_t alert_for(int e) { switch (e) { case TLS_ERR_UNSUPPORTED: return ALERT_HANDSHAKE_FAIL; case TLS_ERR_CERT: case TLS_ERR_NAME: return ALERT_BAD_CERT; case TLS_ERR_SIG: case TLS_ERR_FINISHED: return ALERT_DECRYPT;
                                         case TLS_ERR_OVERFLOW: return ALERT_OVERFLOW; default: return ALERT_UNEXPECTED; } }

static int record_rx(tls_t *t)
{
    uint8_t *r = t->rbuf; unsigned type = r[0], len = be16(r + 3), n;
    if (r[1] != 3) return TLS_ERR_PROTO;
    if (type == 20) return len == 1 && r[5] == 1 ? 0 : TLS_ERR_PROTO;                // compatibility change_cipher_spec: ignored
    uint8_t *body = r + 5; uint8_t inner;
    if (!t->renc) { if (type != 22 && type != 21) return TLS_ERR_PROTO; inner = (uint8_t)type; n = len; }
    else {
        if (type != 23 || len < 17) return TLS_ERR_PROTO;
        uint8_t nonce[12]; nonce_of(t->riv, t->rseq, nonce);
        n = len - 16;
        if (aes_gcm(t->rkey, 16, false, nonce, 12, r, 5, body, body, n, body + n)) return TLS_ERR_MAC;
        t->rseq++;
        while (n && !body[n - 1]) n--;                                               // inner padding
        if (!n) return TLS_ERR_PROTO;
        inner = body[--n];
        if (n > TLS_REC_PLAIN) return TLS_ERR_OVERFLOW;
    }
    switch (inner) {
    case 22: return handshake_rx(t, body, n);
    case 21:
        if (n != 2) return TLS_ERR_PROTO;
        if (body[1] == 0) { if (t->state == TLS_CONNECTED) { t->state = TLS_CLOSED; return 0; } return TLS_ERR_ALERT; }
        return TLS_ERR_ALERT;
    case 23:
        if (t->state != TLS_CONNECTED) return TLS_ERR_PROTO;
        t->rplain_off = (uint16_t)(body - r); t->rplain_len = (uint16_t)n; return 0;
    default: return TLS_ERR_PROTO;
    }
}

void tls_server_init(tls_t *t, const tls_server_cfg_t *c) { memset(t, 0, sizeof *t); t->is_server = 1; t->scfg = c; t->hs = S_WAIT_CH; sha_start(&t->th, SHA256); }

int tls_feed(tls_t *t, const uint8_t *in, unsigned n)
{
    unsigned used = 0;
    if (t->state == TLS_FAILED) return t->err;
    while (used < n) {
        if (t->rplain_len) break;                                                    // application data waits for tls_read: no new record until it is drained
        unsigned want;
        if (t->rfill < 5) want = 5 - t->rfill;
        else { unsigned rl = be16(t->rbuf + 3); if (rl > TLS_REC_BUF - 5) { fail(t, TLS_ERR_OVERFLOW, ALERT_OVERFLOW); return t->err; } want = 5 + rl - t->rfill; }
        unsigned take = n - used < want ? n - used : want;
        memcpy(t->rbuf + t->rfill, in + used, take); t->rfill = (uint16_t)(t->rfill + take); used += take;
        if (t->rfill >= 5 && t->rfill == 5 + be16(t->rbuf + 3)) {
            unsigned rl = be16(t->rbuf + 3);
            if (rl > TLS_REC_BUF - 5) { fail(t, TLS_ERR_OVERFLOW, ALERT_OVERFLOW); return t->err; }
            t->rfill = 0;
            int r = record_rx(t);
            if (r) { fail(t, r, r == TLS_ERR_MAC ? ALERT_BAD_MAC : alert_for(r)); return t->err; }
            if (t->state == TLS_CLOSED) break;
        } else if (t->rfill == 5 && be16(t->rbuf + 3) > TLS_REC_BUF - 5) { fail(t, TLS_ERR_OVERFLOW, ALERT_OVERFLOW); return t->err; }
    }
    return (int)used;
}

const uint8_t *tls_out(tls_t *t, unsigned *n) { *n = t->wlen; return t->wbuf; }
void tls_out_done(tls_t *t, unsigned n) { if (n > t->wlen) n = t->wlen; memmove(t->wbuf, t->wbuf + n, t->wlen - n); t->wlen = (uint16_t)(t->wlen - n); }

int tls_write(tls_t *t, const void *p, unsigned n)
{
    if (t->state != TLS_CONNECTED) return 0;
    if (n > TLS_REC_PLAIN) n = TLS_REC_PLAIN;
    unsigned room = sizeof t->wbuf - t->wlen; if (room < 5 + 17 + 1) return 0;
    if (n > room - 5 - 17) n = room - 5 - 17;
    if (!n) return 0;
    put_record(t, 23, p, n);
    return (int)n;
}

int tls_read(tls_t *t, void *p, unsigned max)
{
    if (t->rplain_len) {
        unsigned n = t->rplain_len < max ? t->rplain_len : max;
        memcpy(p, t->rbuf + t->rplain_off, n); t->rplain_off = (uint16_t)(t->rplain_off + n); t->rplain_len = (uint16_t)(t->rplain_len - n);
        return (int)n;
    }
    if (t->state == TLS_FAILED) return t->err;
    return t->state == TLS_CLOSED ? TLS_ERR_CLOSED : 0;
}

int tls_state(const tls_t *t) { return t->state; }
void tls_close(tls_t *t) { if (t->state == TLS_CONNECTED) { uint8_t a[2] = { 1, 0 }; put_record(t, 21, a, 2); t->state = TLS_CLOSED; } }
