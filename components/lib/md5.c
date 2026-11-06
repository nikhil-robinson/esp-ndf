/* MD5 (RFC 1321), used to verify the partition table checksum entry. */
#include "md5.h"
#include "soc_caps.h"

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

#if SOC_CPU_RISCV
/* all 64 steps unrolled (the rotate amounts and message words become immediates; the loop above spent 37 cycles per byte) */
#define F1(b, c, d) ((d) ^ ((b) & ((c) ^ (d))))
#define F2(b, c, d) ((c) ^ ((d) & ((b) ^ (c))))
#define F3(b, c, d) ((b) ^ (c) ^ (d))
#define F4(b, c, d) ((c) ^ ((b) | ~(d)))
#define STEP(f, a, b, c, d, g, k, s) do { (a) += f(b, c, d) + w[g] + (k); (a) = (b) + (((a) << (s)) | ((a) >> (32 - (s)))); } while (0)
static void block(uint32_t h[4], const uint8_t *p)
{
    uint32_t w[16], a = h[0], b = h[1], c = h[2], d = h[3];
    if (!((uintptr_t)p & 3)) for (int i = 0; i < 16; i++) w[i] = ((const uint32_t *)p)[i];
    else for (int i = 0; i < 16; i++) w[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    STEP(F1, a, b, c, d, 0, 0xd76aa478, 7);  STEP(F1, d, a, b, c, 1, 0xe8c7b756, 12); STEP(F1, c, d, a, b, 2, 0x242070db, 17); STEP(F1, b, c, d, a, 3, 0xc1bdceee, 22);
    STEP(F1, a, b, c, d, 4, 0xf57c0faf, 7);  STEP(F1, d, a, b, c, 5, 0x4787c62a, 12); STEP(F1, c, d, a, b, 6, 0xa8304613, 17); STEP(F1, b, c, d, a, 7, 0xfd469501, 22);
    STEP(F1, a, b, c, d, 8, 0x698098d8, 7);  STEP(F1, d, a, b, c, 9, 0x8b44f7af, 12); STEP(F1, c, d, a, b, 10, 0xffff5bb1, 17); STEP(F1, b, c, d, a, 11, 0x895cd7be, 22);
    STEP(F1, a, b, c, d, 12, 0x6b901122, 7); STEP(F1, d, a, b, c, 13, 0xfd987193, 12); STEP(F1, c, d, a, b, 14, 0xa679438e, 17); STEP(F1, b, c, d, a, 15, 0x49b40821, 22);
    STEP(F2, a, b, c, d, 1, 0xf61e2562, 5);  STEP(F2, d, a, b, c, 6, 0xc040b340, 9);  STEP(F2, c, d, a, b, 11, 0x265e5a51, 14); STEP(F2, b, c, d, a, 0, 0xe9b6c7aa, 20);
    STEP(F2, a, b, c, d, 5, 0xd62f105d, 5);  STEP(F2, d, a, b, c, 10, 0x02441453, 9); STEP(F2, c, d, a, b, 15, 0xd8a1e681, 14); STEP(F2, b, c, d, a, 4, 0xe7d3fbc8, 20);
    STEP(F2, a, b, c, d, 9, 0x21e1cde6, 5);  STEP(F2, d, a, b, c, 14, 0xc33707d6, 9); STEP(F2, c, d, a, b, 3, 0xf4d50d87, 14);  STEP(F2, b, c, d, a, 8, 0x455a14ed, 20);
    STEP(F2, a, b, c, d, 13, 0xa9e3e905, 5); STEP(F2, d, a, b, c, 2, 0xfcefa3f8, 9);  STEP(F2, c, d, a, b, 7, 0x676f02d9, 14);  STEP(F2, b, c, d, a, 12, 0x8d2a4c8a, 20);
    STEP(F3, a, b, c, d, 5, 0xfffa3942, 4);  STEP(F3, d, a, b, c, 8, 0x8771f681, 11); STEP(F3, c, d, a, b, 11, 0x6d9d6122, 16); STEP(F3, b, c, d, a, 14, 0xfde5380c, 23);
    STEP(F3, a, b, c, d, 1, 0xa4beea44, 4);  STEP(F3, d, a, b, c, 4, 0x4bdecfa9, 11); STEP(F3, c, d, a, b, 7, 0xf6bb4b60, 16);  STEP(F3, b, c, d, a, 10, 0xbebfbc70, 23);
    STEP(F3, a, b, c, d, 13, 0x289b7ec6, 4); STEP(F3, d, a, b, c, 0, 0xeaa127fa, 11); STEP(F3, c, d, a, b, 3, 0xd4ef3085, 16);  STEP(F3, b, c, d, a, 6, 0x04881d05, 23);
    STEP(F3, a, b, c, d, 9, 0xd9d4d039, 4);  STEP(F3, d, a, b, c, 12, 0xe6db99e5, 11); STEP(F3, c, d, a, b, 15, 0x1fa27cf8, 16); STEP(F3, b, c, d, a, 2, 0xc4ac5665, 23);
    STEP(F4, a, b, c, d, 0, 0xf4292244, 6);  STEP(F4, d, a, b, c, 7, 0x432aff97, 10); STEP(F4, c, d, a, b, 14, 0xab9423a7, 15); STEP(F4, b, c, d, a, 5, 0xfc93a039, 21);
    STEP(F4, a, b, c, d, 12, 0x655b59c3, 6); STEP(F4, d, a, b, c, 3, 0x8f0ccc92, 10); STEP(F4, c, d, a, b, 10, 0xffeff47d, 15); STEP(F4, b, c, d, a, 1, 0x85845dd1, 21);
    STEP(F4, a, b, c, d, 8, 0x6fa87e4f, 6);  STEP(F4, d, a, b, c, 15, 0xfe2ce6e0, 10); STEP(F4, c, d, a, b, 6, 0xa3014314, 15); STEP(F4, b, c, d, a, 13, 0x4e0811a1, 21);
    STEP(F4, a, b, c, d, 4, 0xf7537e82, 6);  STEP(F4, d, a, b, c, 11, 0xbd3af235, 10); STEP(F4, c, d, a, b, 2, 0x2ad7d2bb, 15); STEP(F4, b, c, d, a, 9, 0xeb86d391, 21);
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}
#else
static void block(uint32_t h[4], const uint8_t *p)
{
    uint32_t w[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++) w[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
        uint32_t t = d; d = c; c = b;
        uint32_t x = a + f + K[i] + w[g];
        b += x << R[i] | x >> (32 - R[i]);
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

#endif

void md5(const void *data, size_t len, uint8_t out[16])
{
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    const uint8_t *p = data;
    size_t n = len;
    for (; n >= 64; n -= 64, p += 64) block(h, p);
    uint8_t tail[128] = { 0 };
    for (size_t i = 0; i < n; i++) tail[i] = p[i];
    tail[n] = 0x80;
    size_t tl = n < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[tl - 8 + i] = bits >> (8 * i);
    block(h, tail);
    if (tl == 128) block(h, tail + 64);
    for (int i = 0; i < 16; i++) out[i] = h[i / 4] >> (8 * (i & 3));
}
