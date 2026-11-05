#include "test.h"
#include "spi.h"
#include "gpio.h"
#include "sys.h"

/* 1) master loopback: MOSI GPIO10 -> jumper -> MISO GPIO11; SCLK on GPIO6 (analyzer D2), CS GPIO4 (D1)
 * 2) master SPI2 <-> slave SPI3: MOSI 10->11 and SCLK 12->13 over jumpers, CS GPIO4 and MISO GPIO5 shared pads */
#define LEN 4096
static uint8_t tx[LEN], rx[LEN];

static int check(size_t n) { int bad = 0; for (size_t i = 0; i < n; i++) bad += rx[i] != tx[i]; return bad; }

void test_spi(void)
{
    for (int i = 0; i < LEN; i++) tx[i] = i * 13 + 7;
    static const uint32_t f[3] = { 10000000, 20000000, 40000000 };
    for (int k = 0; k < 3; k++) {
        uint32_t act = spi_master_init(2, 6, 10, 11, 4, f[k], 0);
        for (int i = 0; i < LEN; i++) rx[i] = 0;
        uint32_t t = ccount();
        spi_xfer(2, tx, rx, 64);
        uint32_t c64 = ccount() - t;
        int b64 = check(64);
        t = ccount();
        spi_xfer(2, tx, rx, LEN);
        uint32_t us = (ccount() - t) / (sys_cpu_hz() / 1000000);
        int b4k = check(LEN);
        printf("    %2lu MHz: 64 B PIO %d wrong (%lu cycles), 4 KB DMA %d wrong, %lu us (wire %lu us)\n",
               act / 1000000, b64, c64, b4k, us, (uint32_t)((uint64_t)LEN * 8 * 1000000 / act));
        CHECK(act == f[k] && b64 == 0 && b4k == 0);
    }
    METRIC("spi_xfer() 4 bytes", "%lu cycles", CYCLES(spi_xfer(2, tx, rx, 4)));
    spi_master_init(2, 6, 10, 11, 4, 40000000, 0);
    METRIC("spi_xfer() 4 bytes @40 MHz (wire 192)", "%lu cycles", CYCLES(spi_xfer(2, tx, rx, 4)));
    METRIC("spi_xfer() 32 bytes @40 MHz (wire 1536)", "%lu cycles", CYCLES(spi_xfer(2, tx, rx, 32)));
    spi_master_init(2, 6, 10, 11, 4, 10000000, 0);
    printf("    analyzer: SPI 10 MHz burst\n");
    spi_master_init(2, 6, 10, 11, 4, 10000000, 0);
    for (int i = 0; i < 2000; i++) spi_xfer(2, tx, rx, 64);

    /* master <-> slave, 32 bytes full duplex each way, all 4 modes */
    for (int mode = 0; mode < 4; mode++) {
        uint8_t s_tx[32], s_rx[32];
        for (int i = 0; i < 32; i++) s_tx[i] = 0xC0 + i;
        spi_master_init(2, 12, 10, 5, 4, 10000000, mode);
        spi_slave_init(3, 13, 11, 5, 4, mode);
        spi_slave_arm(3, s_tx, 32);
        for (int i = 0; i < 32; i++) rx[i] = 0;
        spi_xfer(2, tx, rx, 32);
        delay_us(10);
        int n = spi_slave_result(3, s_rx, 32), bm = 0, bs = 0;
        for (int i = 0; i < 32; i++) { bs += s_rx[i] != tx[i]; bm += rx[i] != s_tx[i]; }
        printf("    mode %d master<->slave: slave got %d B (%d wrong), master got %d wrong\n", mode, n, bs, bm);
        CHECK(n == 32 && bs == 0 && bm == 0);
    }
}

NDF_UNIT(010, spi);
