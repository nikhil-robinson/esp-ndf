/* Hardware RNG (TRM 25) + ChaCha20 generator (RFC 8439 block function). Hardware side as ESP-IDF
 * (bootloader_random_esp32s3.c: RNG clock, 8 MHz clock source; hw_random.c: XOR of reads, 1778 APB cycles between
 * words). Generator as OpenBSD arc4random: a 1 KB keystream buffer (16 ChaCha20 blocks); its first 32 bytes become the
 * next key at once and every byte handed out is zeroed, so earlier output cannot be recomputed from the state (fast
 * key erasure); calls are served from the buffer. */
#include "rng.h"
#include "cpu.h"
#include "sys.h"
#include <string.h>

#define WDEV_RND      0x6003507Cu
#define WIFI_CLK_EN   0x60026014u     /* SYSCON: b15 RNG clock */
#define RTC_CLK_CONF  0x60008074u     /* b10 8 MHz clock to the digital domain (RNG entropy source) */

static bool up;
static uint32_t last;
static uint32_t key[8], ctr;          /* generator state */
static uint32_t since_seed;
#define RSBUF 1024
static uint8_t  buf[RSBUF] __attribute__((aligned(4))); static unsigned avail;   /* unused keystream: buf[RSBUF - avail..] */

/* Boot seeding with the SAR ADC noise source switched on (IDF bootloader_random_enable / _disable for the S3, register
 * for register): the RC 8 MHz source alone is not full entropy without RF; with the ADC sampling its internal reference
 * the RNG words are. Runs once before main (no ADC user yet); the ADC is put back in reset afterwards. */
#define SAR(o)  (0x60040000u + (o))
#define SENS(o) (0x60008800u + (o))
#define PERIP_CLK_EN0 0x600C0018u
#define PERIP_RST_EN0 0x600C0020u
#include "../rf/rf_regi2c.h"
static void sar_entropy(bool on)
{
    if (on) {
        REG_SET(PERIP_CLK_EN0, BIT(28)); REG_CLR(PERIP_CLK_EN0, BIT(28));     /* as IDF: pulse the clock enable */
        REG_FIELD(SAR(0x70), 21, 2, 2);                                       /* CLKM: APB */
        REG_SET(SAR(0x00), BIT(6));                                           /* SAR clock gated */
        REG_SET(SAR(0x70), BIT(20));                                          /* CLKM enable */
        REG_FIELD(SAR(0x70), 0, 8, 3);
        REG_FIELD(SAR(0x00), 7, 8, 3);
        REG_FIELD(SAR(0x04), 12, 12, 70);                                     /* timer target */
        REG_CLR(SAR(0x00), BIT(0));                                           /* no start force */
        REG_FIELD(SENS(0x3C), 29, 2, 3);                                      /* force XPD SAR */
        REG_CLR(SAR(0x04), BIT(0));                                           /* no meas limit */
        REG_FIELD(SAR(0x00), 3, 2, 1);                                        /* work mode */
        REG_FIELD(SAR(0x00), 19, 4, 0); REG(SAR(0x28)) = 0xAFFFFF;            /* patterns: internal voltage (0xA) */
        REG_FIELD(SAR(0x00), 15, 4, 0); REG(SAR(0x18)) = 0xAFFFFF;
        REG_SET(SENS(0x10), BIT(31)); REG_CLR(SENS(0x34), BIT(31));
        REG_CLR(SAR(0x38), BIT(5)); REG_CLR(SAR(0x38), BIT(12));
        REG_FIELD(SAR(0x3C), 19, 5, 0xD); REG_FIELD(SAR(0x3C), 14, 5, 0xD);
        REG_SET(SAR(0x04), BIT(11)); REG_SET(SAR(0x04), BIT(24));             /* timer select + enable */
        rf_regi2c_write_bits(0x69, 7, 4, 4, 1); rf_regi2c_write_bits(0x69, 7, 2, 2, 1);
        rf_regi2c_write_bits(0x69, 7, 3, 3, 0); rf_regi2c_write_bits(0x69, 7, 1, 0, 0);
    } else {
        rf_regi2c_write_bits(0x69, 7, 4, 4, 0); rf_regi2c_write_bits(0x69, 7, 2, 2, 0);
        rf_regi2c_write_bits(0x69, 7, 3, 3, 0); rf_regi2c_write_bits(0x69, 7, 1, 0, 0);
        REG_FIELD(SENS(0x3C), 29, 2, 0);
        REG_CLR(SENS(0x10), BIT(31));
        REG_CLR(SAR(0x04), BIT(24));
        REG_CLR(PERIP_CLK_EN0, BIT(28));
        REG_SET(PERIP_RST_EN0, BIT(28));
    }
}

static bool seeded;
void rng_boot_seed(void)       /* first use of rng_fill / rng_u32, not at boot: ~1 ms of paced hardware words that no
                                * application without keys should pay on every reset */
{
    rng_init();
    sar_entropy(true);
    for (int r = 0; r < 4; r++) for (int i = 0; i < 8; i++) key[i] ^= rng_hw();   /* 1024 bits of hardware words */
    sar_entropy(false);
    since_seed = 0; seeded = true;
}

void rng_init(void)
{
    REG_SET(WIFI_CLK_EN, BIT(15));
    REG_SET(RTC_CLK_CONF, BIT(10));
    last = ccount();
    up = true;
}

uint32_t rng_hw(void)
{
    if (!up) rng_init();
    uint32_t wait = 1778 * (sys_cpu_hz() / 80000000u), r = 0, c;   /* 1778 APB cycles (IDF: tested rate) */
    do { c = ccount(); r ^= REG(WDEV_RND); } while (c - last < wait);
    last = c;
    return r;
}

#define ROTL(x, n) ((x) << (n) | (x) >> (32 - (n)))
#define QR(a, b, c, d) (a += b, d ^= a, d = ROTL(d, 16), c += d, b ^= c, b = ROTL(b, 12), \
                        a += b, d ^= a, d = ROTL(d, 8), c += d, b ^= c, b = ROTL(b, 7))

static void chacha(const uint32_t k[8], uint32_t n0, const uint32_t nonce[3], uint32_t out[16])
{
    uint32_t x[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574, k[0], k[1], k[2], k[3],
                       k[4], k[5], k[6], k[7], n0, nonce[0], nonce[1], nonce[2] };
    uint32_t in[16]; memcpy(in, x, sizeof in);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

static const uint32_t zero_nonce[3];
static void chacha_block(uint32_t out[16]) { chacha(key, ctr++, zero_nonce, out); }

int rng_selftest(void)                 /* RFC 8439 2.3.2: key 00..1f, counter 1, nonce 000000090000004a00000000 */
{
    uint32_t k[8], o[16];
    static const uint32_t n[3] = { 0x09000000, 0x4a000000, 0 };
    for (int i = 0; i < 8; i++) k[i] = 0x03020100u + 0x04040404u * i;
    chacha(k, 1, n, o);
    return o[0] == 0xe4e7f110 && o[1] == 0x15593bd1 && o[15] == 0x4e3c50a2 ? 0 : -1;
}

static void refill(void)               /* new buffer; its first 32 bytes are the next key (then erased) */
{
    for (int i = 0; i < RSBUF / 64; i++) chacha_block((uint32_t *)(buf + 64 * i));
    memcpy(key, buf, 32);
    memset(buf, 0, 32);
    avail = RSBUF - 32;
}

static void reseed(void)
{
    for (int i = 0; i < 8; i++) key[i] ^= rng_hw();
    since_seed = 0;
    refill();
}

void rng_fill(void *out, size_t len)
{
    uint8_t *o = out;
    uint32_t st = irq_lock();
    static uint32_t lk;
    while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) {}       /* the other core */
    if (!seeded) { rng_boot_seed(); refill(); }                    /* first use: full seeding (ADC noise + 1024 hardware bits) */
    else if (since_seed >= NDF_RNG_RESEED) reseed();
    since_seed += len;
    while (len) {
        if (!avail) refill();
        unsigned n = len < avail ? len : avail;
        uint8_t *p = buf + RSBUF - avail;
        memcpy(o, p, n);
        memset(p, 0, n);                                           /* handed-out keystream does not stay in memory */
        avail -= n; o += n; len -= n;
    }
    __atomic_store_n(&lk, 0, __ATOMIC_RELEASE);
    irq_unlock(st);
}

uint32_t rng_u32(void) { uint32_t v; rng_fill(&v, 4); return v; }
