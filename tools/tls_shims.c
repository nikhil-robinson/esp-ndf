/* Host stand-ins for the board's SHA-256 engine, AES-GCM and rng (software SHA-256, OpenSSL AES-GCM). Included by tls_host.c and fuzz_tls.c. */
#include <string.h>
#include <stdlib.h>
#include "sha.h"
#include "aes.h"
#include "rng.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
/* --- host stand-ins --- */
static const uint32_t K[64] = { 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void blk(uint32_t *h, const uint8_t *p)
{
    uint32_t w[64], a[8]; memcpy(a, h, 32);
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) { uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10); w[i] = w[i-16] + s0 + w[i-7] + s1; }
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25), ch = (a[4] & a[5]) ^ (~a[4] & a[6]), t1 = a[7] + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22), mj = (a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]), t2 = S0 + mj;
        a[7] = a[6]; a[6] = a[5]; a[5] = a[4]; a[4] = a[3] + t1; a[3] = a[2]; a[2] = a[1]; a[1] = a[0]; a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += a[i];
}
void sha_start(sha_ctx_t *c, sha_type_t t) { (void)t; memset(c, 0, sizeof *c); static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 }; memcpy(c->h, iv, 32); }
void sha_update(sha_ctx_t *c, const void *d, size_t n)
{
    const uint8_t *p = d; c->total += n;
    while (n) { size_t k = 64 - c->fill; if (k > n) k = n; memcpy(c->buf + c->fill, p, k); c->fill += (uint32_t)k; p += k; n -= k; if (c->fill == 64) { blk(c->h, c->buf); c->fill = 0; } }
}
void sha_finish(sha_ctx_t *c, uint8_t *out)
{
    uint64_t bits = c->total * 8; uint8_t z = 0x80; sha_update(c, &z, 1); z = 0; while (c->fill != 56) sha_update(c, &z, 1);
    uint8_t l[8]; for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i)); sha_update(c, l, 8);
    for (int i = 0; i < 8; i++) { out[4*i] = (uint8_t)(c->h[i] >> 24); out[4*i+1] = (uint8_t)(c->h[i] >> 16); out[4*i+2] = (uint8_t)(c->h[i] >> 8); out[4*i+3] = (uint8_t)c->h[i]; }
}
size_t sha_size(sha_type_t t) { (void)t; return 32; }
void sha_invalidate(void) {}
void sha(sha_type_t t, const void *d, size_t n, uint8_t *o) { sha_ctx_t c; sha_start(&c, t); sha_update(&c, d, n); sha_finish(&c, o); }
void rng_fill(void *b, size_t n) { RAND_bytes(b, (int)n); }
int aes_gcm(const uint8_t *key, int kl, bool enc, const uint8_t *iv, size_t il, const void *aad, size_t al, const void *in, void *out, size_t len, uint8_t tag[16])
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); int n, ok = 1; uint8_t fin[16];
    EVP_CipherInit_ex(c, kl == 16 ? EVP_aes_128_gcm() : EVP_aes_256_gcm(), 0, 0, 0, enc);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, (int)il, 0); EVP_CipherInit_ex(c, 0, 0, key, iv, enc);
    if (!enc) EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag);
    if (al) EVP_CipherUpdate(c, 0, &n, aad, (int)al);
    if (len) EVP_CipherUpdate(c, out, &n, in, (int)len);
    if (enc) { EVP_CipherFinal_ex(c, fin, &n); EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag); }
    else if (EVP_CipherFinal_ex(c, fin, &n) <= 0) { memset(out, 0, len); ok = 0; }
    EVP_CIPHER_CTX_free(c); return ok ? 0 : -1;
}

