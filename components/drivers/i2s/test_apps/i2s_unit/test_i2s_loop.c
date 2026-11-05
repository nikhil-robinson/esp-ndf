#include "test.h"
#include "i2s.h"
#include "gpio.h"
#include "sys.h"

/* One port full duplex (the RX half is a slave on the TX half's BCK / WS, IDF sig_loopback), 16-bit Philips, DOUT back in on
 * the same pad: no jumper needed. A counter goes out, every received sample must be the previous one + 1. */
static uint16_t out[512], in[2048];

void test_i2s_loop(void)
{
    irq_enable_all();
    i2s_cfg_t c = { .rate = 48000, .bits = 16, .mclk = -1, .bck = 12, .ws = 4, .dout = 10, .din = 10 };
    uint64_t fs = i2s_init(0, &c);
    CHECK(fs > 47900000 && fs < 48100000);
    uint16_t v = 1;
    for (int k = 0; k < 4; k++) { for (int i = 0; i < 512; i++) out[i] = v++; i2s_write(0, out, sizeof out, 100000); }
    i2s_start(0, true, true);
    uint32_t got = 0, breaks = 0, seen = 0;
    uint16_t prev = 0;
    uint32_t t0 = ccount();
    while (got < 64 * 1024 && ccount() - t0 < sys_cpu_hz() * 2) {
        for (int i = 0; i < 512; i++) out[i] = v++;
        i2s_write(0, out, sizeof out, 100000);
        int n = i2s_read(0, in, sizeof in, 0);
        for (int i = 0; i < n / 2; i++) {
            if (in[i] == 0) continue;
            if (seen && in[i] != (uint16_t)(prev + 1)) breaks++;
            prev = in[i]; seen++;
        }
        got += n;
    }
    i2s_stop(0);
    printf("    duplex loop: %lu bytes, %lu samples checked, %lu discontinuities, tx underruns %lu, rx overruns %lu\n",
           got, seen, breaks, i2s_tx_underruns(0), i2s_rx_overruns(0));
    CHECK(seen > 20000 && breaks == 0 && i2s_rx_overruns(0) == 0);
}

NDF_UNIT(005, i2s_loop);
