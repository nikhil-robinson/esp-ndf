#include "test.h"
#include "i2s.h"
#include "gpio.h"
#include "sys.h"

/* I2S0 TX master: BCK GPIO12 -> jumper -> GPIO13, DOUT GPIO10 -> jumper -> GPIO11, WS GPIO4 (pad, analyzer D1)
 * I2S1 RX slave on 13 / 11 / 4. Stream a 16-bit counter, check every sample is +1. */
static uint16_t out[512], in[2048];

void test_i2s(void)
{
    irq_enable_all();
    i2s_cfg_t tx = { .rate = 48000, .bits = 16, .mclk = -1, .bck = 12, .ws = 4, .dout = 10, .din = -1 };
    i2s_cfg_t rx = { .rate = 48000, .bits = 16, .slave = true, .mclk = -1, .bck = 13, .ws = 4, .dout = -1, .din = 11 };
    uint64_t fs = i2s_init(0, &tx);
    i2s_init(1, &rx);
    gpio_config(6, GPIO_OUTPUT); gpio_out_signal(6, 22, false);       /* BCK mirror for analyzer D2 */
    printf("    48 kHz requested, achieved %lu.%03lu Hz\n", (uint32_t)(fs / 1000), (uint32_t)(fs % 1000));
    uint16_t v = 1;
    for (int k = 0; k < 4; k++) {                                     /* pre-fill before starting */
        for (int i = 0; i < 512; i++) out[i] = v++;
        i2s_write(0, out, sizeof out, 100000);
    }
    i2s_start(1, false, true);
    i2s_start(0, true, false);
    printf("    analyzer: I2S streaming\n");
    uint32_t got = 0, breaks = 0, seen = 0;
    uint16_t prev = 0;
    uint32_t t0 = ccount();
    while (got < 64 * 1024 && ccount() - t0 < sys_cpu_hz() * 2) {      /* 64 KB = ~0.34 s */
        for (int i = 0; i < 512; i++) out[i] = v++;
        i2s_write(0, out, sizeof out, 100000);
        int n = i2s_read(1, in, sizeof in, 0);
        for (int i = 0; i < n / 2; i++) {
            if (in[i] == 0) continue;                                 /* leading silence */
            if (seen && in[i] != (uint16_t)(prev + 1)) breaks++;
            prev = in[i]; seen++;
        }
        got += n;
    }
    printf("    received %lu bytes, %lu samples checked, %lu discontinuities, tx underruns %lu, rx overruns %lu\n",
           got, seen, breaks, i2s_tx_underruns(0), i2s_rx_overruns(1));
    CHECK(seen > 20000 && breaks == 0 && i2s_rx_overruns(1) == 0);
    METRIC("i2s_write() 1 KB (space free)", "%lu cycles", ({ uint32_t c0 = 0; i2s_read(1, in, sizeof in, 0); while (i2s_read(1, in, sizeof in, 0) == 0 && c0++ < 100000) {} uint32_t t = ccount(); i2s_write(0, out, 1024, 100000); ccount() - t; }));
    i2s_stop(0); i2s_stop(1);
}

NDF_UNIT(010, i2s);
