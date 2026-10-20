/* Test of the public RF API (include/rf.h) on C9:48, with D0 running rfctl as the other end
 * (tools/rf_api_test.py drives both). Prints "STEP n" when the host must act, "PASS"/"FAIL" lines. */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "rf.h"
#include <math.h>

static int fails;
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
    check("tx_stop", rf_tx_stop() == RF_OK && rf_state() == RF_STATE_IDLE);

    /* hop */
    rf_hop_t a, b;
    check("hop prepare 2402/2480", rf_hop_prepare(&a, 2402000) == RF_OK && rf_hop_prepare(&b, 2480000) == RF_OK);
    uint32_t c0 = ccount(); rf_hop(&a); uint32_t c1 = ccount(); rf_hop(&b); uint32_t c2 = ccount();
    printf("hop %lu us, %lu us\n", (unsigned long)((c1 - c0) / 240), (unsigned long)((c2 - c1) / 240));
    check("hop < 60 us", (c1 - c0) / 240 < 60 && (c2 - c1) / 240 < 60);
    rf_close();
    check("close", rf_state() == RF_STATE_OFF);
    printf("=== %d failures\n=== done\n", fails);
    for (;;) {}
}
