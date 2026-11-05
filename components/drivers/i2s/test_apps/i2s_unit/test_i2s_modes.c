#include "test.h"
#include "i2s.h"
#include "gpio.h"
#include "fastmath.h"
#include "sys.h"

/* Same jumpers as test_i2s: GPIO12 -> 13 (clock), GPIO10 -> 11 (data), WS on GPIO4.
 * TDM: I2S0 TX 4 slots x 16 bit, slot 2 masked off; I2S1 RX slave 4 slots. Sample values count
 *      up; the masked slot takes no buffer data, so the stream arrives in order with a 0 in every 4th slot.
 * PDM: I2S0 full duplex (PDM is I2S0 only): PCM->PDM 48 kHz, clock 6.144 MHz on GPIO12, data
 *      10 -> 11 back into PDM->PCM (decimation 128). A 1 kHz sine must come back at 1 kHz. */
static int16_t out[512], in[2048];

static void tdm(void)
{
    if (!(test_jumper(12, 13) && test_jumper(10, 11))) { printf("    SKIP TDM: no jumpers GPIO12<->13, 10<->11\n"); return; }
    i2s_cfg_t tx = { .mode = I2S_TDM, .slots = 4, .mask = 0xB, .rate = 48000, .bits = 16,
                     .mclk = -1, .bck = 12, .ws = 4, .dout = 10, .din = -1 };
    i2s_cfg_t rx = { .mode = I2S_TDM, .slots = 4, .rate = 48000, .bits = 16, .slave = true,
                     .mclk = -1, .bck = 13, .ws = 4, .dout = -1, .din = 11 };
    uint32_t t = ccount();
    uint64_t fs = i2s_init(0, &tx);
    t = ccount() - t;
    CHECK(fs && i2s_init(1, &rx));
    METRIC("i2s_init() TDM TX", "%lu cycles", t);
    printf("    TDM 4x16: fs %lu.%03lu Hz (BCK 3.072 MHz)\n", (uint32_t)(fs / 1000), (uint32_t)(fs % 1000));
    uint16_t v = 1;
    for (int k = 0; k < 4; k++) { for (int i = 0; i < 512; i++) out[i] = v++; i2s_write(0, out, sizeof out, 100000); }
    i2s_start(1, false, true); i2s_start(0, true, false);
    uint32_t seen = 0, zeros = 0, bad = 0, got = 0, t0 = ccount();
    uint16_t prev = 0;
    while (got < 64 * 1024 && ccount() - t0 < sys_cpu_hz() * 2) {
        for (int i = 0; i < 512; i++) out[i] = v++;
        i2s_write(0, out, sizeof out, 100000);
        int n = i2s_read(1, in, sizeof in, 0);
        for (int i = 0; i < n / 2; i++) {
            uint16_t x = in[i];
            if (!seen && !x) continue;                                /* leading silence */
            if (!x) { zeros++; continue; }
            if (seen && x != (uint16_t)(prev + 1)) { if (bad++ < 6) printf("    [%lu] %u -> %u\n", seen, prev, x); }             /* active slots carry the buffer in order */
            prev = x; seen++;
        }
        got += n;
    }
    i2s_stop(0); i2s_stop(1);
    printf("    TDM: %lu samples, %lu masked zeros, %lu errors, und %lu ovr %lu\n", seen, zeros, bad, i2s_tx_underruns(0), i2s_rx_overruns(1));
    CHECK(seen > 20000 && bad == 0 && zeros * 3 >= seen - 3 && zeros * 3 <= seen + 3);
}

static void pdm(void)
{
    i2s_cfg_t c = { .mode = I2S_PDM, .rate = 48000, .bits = 16, .mclk = -1, .bck = -1, .ws = 12, .dout = 10, .din = -1 };
    c.din = test_jumper(10, 11) ? 11 : 10;                  /* PDM data back in on the same pad when the board has no jumper */
    uint32_t t = ccount();
    uint64_t fs = i2s_init(0, &c);
    t = ccount() - t;
    CHECK(fs);
    METRIC("i2s_init() PDM TX+RX", "%lu cycles", t);
    printf("    PDM duplex: fs %lu.%03lu Hz (integer divider as IDF: PDM clock = 128 fs)\n", (uint32_t)(fs / 1000), (uint32_t)(fs % 1000));
    uint32_t ph = 0;
    #define FILL() for (int i = 0; i < 512; i += 2, ph = (ph + 1) % 48) out[i] = out[i + 1] = (int16_t)(12000 * fm_sinf(ph * (6.2831853f / 48)))
    for (int k = 0; k < 4; k++) { FILL(); i2s_write(0, out, sizeof out, 100000); }
    i2s_start(0, true, true);
    uint32_t got = 0, n_s = 0, cross = 0, t0 = ccount();
    int16_t lo = 0, hi = 0; bool neg = false;
    float q1 = 0, q2 = 0, pw = 0;                                     /* Goertzel at 1 kHz / 48 kHz */
    const float cf = 2 * fm_cosf(6.2831853f / 48);
    while (n_s < 48000 / 2 && ccount() - t0 < sys_cpu_hz() * 2) {      /* 0.5 s of left channel */
        FILL(); i2s_write(0, out, sizeof out, 100000);
        int n = i2s_read(0, in, sizeof in, 0);
        for (int i = 0; i < n / 2; i += 2, got++) {
            if (got < 4800) continue;                                 /* 0.1 s for the filters to settle */
            int16_t x = in[i];
            if (x < lo) lo = x;
            if (x > hi) hi = x;
            if (x < -3000) neg = true;                                /* hysteresis: noise near 0 */
            else if (neg && x > 3000) { neg = false; cross++; }
            float q0 = cf * q1 - q2 + x; q2 = q1; q1 = q0; pw += (float)x * x;
            n_s++;
        }
    }
    i2s_stop(0);
    float tone = (q1 * q1 + q2 * q2 - cf * q1 * q2) * 2 / n_s;      /* tone energy */
    int snr = n_s && pw > tone ? (int)(4.3429448f * fm_logf(tone / (pw - tone))) : 99;
    uint32_t hz = n_s ? (uint32_t)((uint64_t)cross * 48000 / n_s) : 0;
    printf("    PDM loop: %lu samples, %lu Hz (1000 sent), peak %d..%d, SNR %d dB, und %lu ovr %lu\n", n_s, hz, lo, hi, snr, i2s_tx_underruns(0), i2s_rx_overruns(0));
    CHECK(n_s > 20000 && hz > 980 && hz < 1020 && hi - lo > 4000 && snr >= 25);
}

/* STD framing formats: I2S0 TX master -> I2S1 RX slave in the same format, 16 kHz 2x16 bit (BCK 512 kHz).
 * The stream must arrive in order, and the WS high time (sampled on GPIO4) must be what the format says:
 * Philips / MSB / PCM long 16 bits, PCM short 1 bit. Cross check: TX Philips into RX MSB must NOT match. */
static uint32_t fmt_run(int ftx, int frx, uint32_t *ws_hi_bits)
{
    i2s_cfg_t tx = { .fmt = ftx, .rate = 16000, .bits = 16, .mclk = -1, .bck = 12, .ws = 4, .dout = 10, .din = -1 };
    i2s_cfg_t rx = { .fmt = frx, .rate = 16000, .bits = 16, .slave = true, .mclk = -1, .bck = 13, .ws = 4, .dout = -1, .din = 11 };
    if (!i2s_init(0, &tx) || !i2s_init(1, &rx)) return ~0u;
    uint16_t v = 1;
    for (int k = 0; k < 4; k++) { for (int i = 0; i < 512; i++) out[i] = v++; i2s_write(0, out, sizeof out, 100000); }
    i2s_start(1, false, true); i2s_start(0, true, false);
    uint32_t seen = 0, bad = 0, t0 = ccount();
    uint16_t prev = 0;
    while (seen < 12000 && ccount() - t0 < sys_cpu_hz()) {
        for (int i = 0; i < 512; i++) out[i] = v++;
        i2s_write(0, out, sizeof out, 100000);
        int n = i2s_read(1, in, sizeof in, 0);
        for (int i = 0; i < n / 2; i++) {
            uint16_t x = in[i];
            if (!seen && !x) continue;
            if (seen && x != (uint16_t)(prev + 1)) bad++;
            prev = x; seen++;
        }
    }
    uint32_t hi = 0, nh = 0, rise = 0;                            /* WS high time, 8 pulses */
    bool last = gpio_read(4);
    for (uint32_t t1 = ccount(); nh < 8 && ccount() - t1 < (sys_cpu_hz() / 10); ) {
        bool w = gpio_read(4);
        uint32_t c = ccount();
        if (w && !last) rise = c;
        if (!w && last && rise) { hi += c - rise; nh++; }
        last = w;
    }
    i2s_stop(0); i2s_stop(1);
    *ws_hi_bits = nh ? (hi / nh + 234) / 469 : 0;                 /* 469 cycles per bit at 512 kHz */
    return seen < 12000 ? ~0u : bad;
}

static void fmts(void)
{
    if (!(test_jumper(12, 13) && test_jumper(10, 11))) { printf("    SKIP formats: no jumpers GPIO12<->13, 10<->11\n"); return; }
    static const char *nm[] = { "Philips", "MSB", "PCM short", "PCM long" };
    static const uint8_t want_ws[] = { 16, 16, 1, 16 };
    for (int f = 0; f < 4; f++) {                             /* back to back: also checks restart (no stale blocks) */
        uint32_t ws, bad = fmt_run(f, f, &ws);
        printf("    %-9s: TX->RX %lu errors, WS high %lu bit(s)\n", nm[f], bad, ws);
        CHECK(bad == 0 && ws == want_ws[f]);
    }
    uint32_t ws, bad = fmt_run(I2S_PHILIPS, I2S_MSB, &ws);
    printf("    TX Philips -> RX MSB: %lu samples out of order (the 1-bit shift is real)\n", bad);
    CHECK(bad > 1000);
}

void test_i2s_modes(void)
{
    irq_enable_all();
    tdm();
    pdm();
    fmts();
}

NDF_UNIT(020, i2s_modes);
