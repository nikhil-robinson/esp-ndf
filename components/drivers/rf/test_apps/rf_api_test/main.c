/* Test of the public RF API (components/include/rf.h) on C9:48, with D0 running rfctl as the other end
 * (tools/rf/rf_api_test.py drives both). Prints "STEP n" when the host must act, "PASS"/"FAIL" lines. */
#include "console.h"
#include "sys.h"
#include "cpu.h"
#include "rf.h"
#include <math.h>
#include <string.h>

static int fails;
static uint32_t stab[16];
static int scb(uint32_t *b, uint32_t n, void *ctx)          /* period, then doubling copies: fits the ~75 us budget */
{
    (void)ctx; memcpy(b, stab, sizeof stab);
    for (uint32_t m = 16; m < n; m *= 2) memcpy(b + m, b, (n - m < m ? n - m : m) * 4);
    return 0;
}
static void check(const char *what, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); fails += !ok; }

static float tone_db(const uint32_t *w, uint32_t n, float f, float fs)
{
    float c = cosf(2 * (float)M_PI * f / fs), s = sinf(2 * (float)M_PI * f / fs), re = 0, im = 0, cr = 1, ci = 0;
    for (uint32_t k = 0; k < n; k++) {
        float i = rf_sample_i(w[k]), q = rf_sample_q(w[k]);
        re += i * cr + q * ci; im += q * cr - i * ci;
        float t = cr * c - ci * s; ci = ci * c + cr * s; cr = t;
        if ((k & 255) == 255) { float m = 1.0f / sqrtf(cr * cr + ci * ci); cr *= m; ci *= m; }
    }
    return 10 * log10f((re * re + im * im) / ((float)n * n) + 1e-6f);
}

static void wait_host(int step) { printf("STEP %d\n", step); while (console_getc() != 'g') {} }

int main(void)
{
    delay_ms(500);
    printf("=== rf_api_test\n");
    check("closed: tx_start -> ERR_STATE", rf_tx_start() == RF_ERR_STATE);
    check("open", rf_open() == RF_OK && rf_state() == RF_STATE_IDLE);
    check("freq out of range -> ERR_RANGE", rf_set_freq_khz(5000000) == RF_ERR_RANGE);

    /* receive: D0 sends 2437 MHz + 2 MHz; we sit at 2436 MHz -> tone at +3 MHz */
    wait_host(1);
    check("set 2436 MHz", rf_set_freq_khz(2436000) == RF_OK);
    check("rx_start", rf_rx_start() == RF_OK);
    rf_rx_set_gain(20);
    const uint32_t *iq; int rc = rf_rx_capture(RF_RATE_80M, 8192, &iq);
    float t = rc ? -99 : tone_db(iq, 8192, 3e6f, 80e6f), fl = rc ? -99 : tone_db(iq, 8192, 7e6f, 80e6f);
    printf("rx tone %.1f dB, floor %.1f dB\n", t, fl);
    check("rx capture sees D0's tone (> 25 dB above floor)", !rc && t - fl > 25);
    int32_t pw; rf_rx_power(255, &pw); printf("rx power estimate %ld\n", (long)pw);

    /* limits */
    check("set 2500 MHz", rf_set_freq_khz(2500000) == RF_OK);
    check("tx outside band -> ERR_DENIED", rf_tx_start() == RF_ERR_DENIED);
    check("level above limit -> ERR_DENIED", rf_tx_set_level_db(1) == RF_ERR_RANGE);

    /* transmit: tone at 2437 MHz + 1 MHz, full level then -10 dB; D0 measures */
    check("set 2437 MHz", rf_set_freq_khz(2437000) == RF_OK);
    check("tx_start", rf_tx_start() == RF_OK);
    check("tone +1 MHz", rf_tx_tone_khz(1000, 300) == RF_OK);
    wait_host(2);
    check("level -10 dB", rf_tx_set_level_db(-10) == RF_OK);
    wait_host(3);
    /* gapless stream: 2.5 MHz tone unit by unit for ~4 s; D0 checks the phase stays continuous */
    rf_tx_set_level_db(0);
    for (int k = 0; k < 16; k++) { float a = 2 * 3.14159265f * k / 16; stab[k] = rf_sample_word((int)(300 * cosf(a)), (int)(300 * sinf(a))); }
    printf("STEP 4\n");
    int src = rf_tx_stream(20000, scb, 0);
    printf("stream rc %d\n", src);
    check("tx_stream 20000 units, no gap", src == RF_OK);
    check("tx_stop", rf_tx_stop() == RF_OK && rf_state() == RF_STATE_IDLE);

    /* hop */
    rf_hop_t a, b;
    check("hop prepare 2402/2480", rf_hop_prepare(&a, 2402000) == RF_OK && rf_hop_prepare(&b, 2480000) == RF_OK);
    uint32_t c0 = ccount(); rf_hop(&a); uint32_t c1 = ccount(); rf_hop(&b); uint32_t c2 = ccount();
    printf("hop %lu us, %lu us\n", (unsigned long)((c1 - c0) / 240), (unsigned long)((c2 - c1) / 240));
    check("hop < 60 us", (c1 - c0) / 240 < 60 && (c2 - c1) / 240 < 60);
    /* lock detection: force the VCO cap far off, detect, recover */
    check("set 2437 MHz", rf_set_freq_khz(2437000) == RF_OK);
    check("locked", rf_pll_locked());
    rf_hop_t bad = { 2437000, 0, 0 }; bad.n = a.n; rf_hop(&bad);        /* cap 0: far out of the window */
    { extern void rf_regi2c_write(uint8_t, uint8_t, uint8_t); rf_regi2c_write(0x62, 0x01, 0); }
    delay_us(50);
    check("unlock detected", !rf_pll_locked());
    check("rf_check_lock recovers", rf_check_lock() == RF_OK && rf_pll_locked() && rf_relock_count() == 1);
    rf_close();
    check("close", rf_state() == RF_STATE_OFF);
    printf("=== %d failures\n=== done\n", fails);
    for (;;) {}
}
