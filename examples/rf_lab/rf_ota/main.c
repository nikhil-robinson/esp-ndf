/* Over-the-air check of our transmit path, two boards at 2437 MHz.
 * Board ...D0 transmits a +2 MHz DAC tone, 1 s on / 1 s off; the other
 * receives and reports the power at +2 / -2 / +5 MHz (control) per capture. */
#include "console.h"
#include "xtensa.h"
#include "sys.h"
#include <stdint.h>
#include <math.h>
#include "rf_pll.h"
#include "rf_init.h"
#include "rf_dump.h"
#include "rf_regi2c.h"
#ifdef COLD_G
#include "rf_state_data/cold_groups.h"
#endif
#ifdef WARM_G
#include "rf_state_data/warm_groups.h"
#endif

static float bin_db(uint32_t n, float f)        /* dBFS-ish power of one bin, 80 MS/s */
{
    const uint32_t *w = (const uint32_t *)RF_DUMP_BURST_ADDR;
    float c = cosf(2 * (float)M_PI * f / 80e6f), s = sinf(2 * (float)M_PI * f / 80e6f), re = 0, im = 0, cr = 1, ci = 0;
    for (uint32_t k = 0; k < n; k++) {
        float i = rf_iq_i(w[k]), q = -rf_iq_q(w[k]);   /* true spectrum */
        re += i * cr + q * ci; im += q * cr - i * ci;
        float t = cr * c - ci * s; ci = ci * c + cr * s; cr = t;
        if ((k & 255) == 255) { float m = 1.0f / sqrtf(cr * cr + ci * ci); cr *= m; ci *= m; }
    }
    return 10 * log10f((re * re + im * im) / ((float)n * n) + 1e-6f);
}

#ifndef OTA_TX_MAC
#define OTA_TX_MAC 0xD0                         /* last MAC byte of the transmitting board */
#endif

int main(void)
{
    delay_ms(1500);
    uint32_t mac0 = *(volatile uint32_t *)0x60007044;
    int tx = (mac0 & 0xFF) == OTA_TX_MAC || (mac0 >> 24) == OTA_TX_MAC;
    printf("=== rf_ota mac0 %08lx role %s\n", (unsigned long)mac0, tx ? "TX" : "RX");
#ifdef COLD_G
    {   /* restore power-on values of one state group (bisecting what our init misses) */
        if (COLD_G == 1) for (unsigned k = 0; k < sizeof cold_g1 / sizeof cold_g1[0]; k++) *(volatile uint32_t *)cold_g1[k].a = cold_g1[k].v;
        if (COLD_G == 2) for (unsigned k = 0; k < sizeof cold_g2 / sizeof cold_g2[0]; k++) *(volatile uint32_t *)cold_g2[k].a = cold_g2[k].v;
        for (unsigned k = 0; k < sizeof cold_g3 / sizeof cold_g3[0]; k++) {
            uint8_t b = cold_g3[k].b;
            int hit = COLD_G == 3 || (COLD_G == 31 && b <= 0x65) || (COLD_G == 32 && b >= 0x66 && b <= 0x6A) || (COLD_G == 33 && b >= 0x6B)
                      || (COLD_G >= 100 && k == COLD_G - 100);
            if (hit) rf_regi2c_write(b, cold_g3[k].r, cold_g3[k].v);
        }
        printf("cold group %d restored\n", COLD_G);
    }
#endif
#ifdef WARM_G
    if (WARM_G & 1) for (unsigned k = 0; k < sizeof warm_g1 / sizeof warm_g1[0]; k++) if (warm_g1[k].a != 0x60008000u || 1) *(volatile uint32_t *)warm_g1[k].a = warm_g1[k].v;
    if (WARM_G & 2) for (unsigned k = 0; k < sizeof warm_g2 / sizeof warm_g2[0]; k++) *(volatile uint32_t *)warm_g2[k].a = warm_g2[k].v;
#endif
#ifdef COLD_SIM
    {   /* software "cold start": RF analog buses and the modem power domain off, then on */
        volatile uint32_t *ana = (volatile uint32_t *)0x60008034, *pwc = (volatile uint32_t *)0x60008090;
        printf("ana_conf before %08lx\n", (unsigned long)*ana);
        *ana &= ~(1u << 31 | 1u << 30 | 1u << 28 | 1u << 27);
        *pwc |= 1u << 17;                       /* WIFI_FORCE_PD */
        delay_ms(200);
        *pwc &= ~(1u << 17);
    }
#endif
    rf_init_replay();
#ifdef WARM_G
    if (WARM_G & 4) for (unsigned k = 0; k < sizeof warm_g3 / sizeof warm_g3[0]; k++) {
        uint8_t b = warm_g3[k].b, r = warm_g3[k].r;
        if ((b == 0x62 && (r == 0x01 || r == 0x02 || r == 0x05)) || b == 0x63) continue;   /* PLL is tuned below */
        rf_regi2c_write(b, r, warm_g3[k].v);
    }
    printf("warm groups %d applied\n", WARM_G);
#endif
#ifndef OTA_TX_MAC
#define OTA_TX_MAC 0xD0                         /* last MAC byte of the transmitting board */
#endif
#ifndef RX_KHZ
#define RX_KHZ (OTA_KHZ - 1000)            /* 1 MHz below: TX leakage at +1, tone at +3, image at -1 MHz */
#endif
#ifndef OTA_KHZ
#define OTA_KHZ 2437000
#endif
    rf_rx_on();
    int trc;
    if (tx) { rf_pll_result_t r; trc = rf_pll_set_khz(OTA_KHZ, &r); if (!trc && !r.locked) trc = -3; }
    else trc = rf_tune_rx_khz(RX_KHZ);
    printf("tune rc %d\n", trc);
    rf_rx_dc_cal(128);
    if (tx) {
        const uint32_t n = 2000;
        volatile uint32_t *b = (volatile uint32_t *)RF_DUMP_BANK(1);
        for (uint32_t k = 0; k < n; k++)
            b[k] = rf_iq_word((int)lrintf(300 * cosf(2 * (float)M_PI * k / 20)), (int)lrintf(300 * sinf(2 * (float)M_PI * k / 20)));
        for (int s = 0; s < 12; s++) {
            int phase = s / 4;                  /* 0 copied DC words, 1 neutral, 2 our DC calibration */
            if (s & 1) { rf_dac_stop(); rf_tx_off(); printf("tx off\n"); delay_ms(1000); continue; }
            if (s == 4) rf_tx_dc_reset();
            if (s == 8) { rf_tx_on(127, 0); uint32_t t = time_ms(); rf_tx_dc_cal(127, 0); printf("tx dc cal %lu ms\n", (unsigned long)(time_ms() - t)); rf_tx_off(); }

            rf_tx_on(127, 0); rf_dac_play(1, n, 0, 1); printf("tx on phase %d\n", phase);
            delay_ms(1000);
        }
        rf_dac_stop(); rf_tx_off();
    } else {
        for (int k = 0; k < 48; k++) {
            rf_rx_gain(20);
            delay_us(200);
            rf_dump_burst(RF_DUMP_80M, 8192);
            printf("t %5lu ms  tone %6.1f dB  leak %6.1f dB  image %6.1f dB  floor %6.1f dB\n", (unsigned long)time_ms(),
                   bin_db(8192, 3e6f), bin_db(8192, 1e6f), bin_db(8192, -1e6f), bin_db(8192, 5e6f));
            delay_ms(250);
        }
    }
    printf("=== done\n");
    for (;;) {}
}
