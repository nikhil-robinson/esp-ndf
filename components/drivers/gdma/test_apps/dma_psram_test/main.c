/* DMA with PSRAM buffers: SPI2 master writes 8 KB from a PSRAM buffer the CPU just filled (lines dirty in the data
 * cache) to the SPI3 HD slave, whose DMA writes into a PSRAM buffer the CPU had read before (lines cached, stale).
 * gdma_out_start / gdma_in_start keep the cache coherent: the received bytes must equal the sent ones. Then the
 * alignment rule: a PSRAM buffer that is not whole cache lines is refused. No jumpers (pads cross-routed). */
#include "console.h"
#include "sys.h"
#include "psram.h"
#include "spi.h"
#include "gpio.h"
#include "gdma.h"
#include <string.h>
#include "lcd_i80.h"
#include "cam.h"
#include "uart.h"
#include "xtensa.h"

static const int8_t mpad[4] = { 16, 17, 18, 21 }, spad[4] = { 1, 2, 42, 47 };
static const uint8_t s2_io[4] = { 103, 102, 105, 104 }, s3_io[4] = { 68, 67, 70, 69 };
#define N 8192

int main(void)
{
    delay_ms(300);
    uint32_t sz = psram_init(2);
    uint8_t *tx = (uint8_t *)PSRAM_VADDR, *rx = (uint8_t *)PSRAM_VADDR + 0x10000;
    printf("psram %lu bytes\n", (unsigned long)sz);
    spi_slave_hd_init(3, 14, 15, spad, 0);
    spi_master_init(2, 14, -1, -1, 15, 10000000, 0);
    for (int i = 0; i < 4; i++) { spi_master_io(2, i, mpad[i]); gpio_in_signal(spad[i], s2_io[i], false); gpio_in_signal(mpad[i], s3_io[i], false); }
    static uint8_t itx[N] __attribute__((aligned(64))), irx[N] __attribute__((aligned(64)));
    int all = 1;
    for (int cs = 0; cs < 3; cs++) {                                  /* 0 PSRAM->int, 1 int->PSRAM, 2 PSRAM->PSRAM */
        uint8_t *t = cs == 1 ? itx : tx, *r = cs == 0 ? irx : rx;
        memset(r, 0x11, N); if (r == rx) psram_writeback(r, N);
        volatile uint32_t sum = 0; for (int i = 0; i < N; i += 4) sum += r[i];   /* r lines cached (0x11) */
        for (int i = 0; i < N; i++) t[i] = (uint8_t)(i * 7 + 3 + cs);           /* t lines dirty */
        spi_hd_t w = { .cmd = SPI_HD_WRDMA | SPI_HD_QUAD, .cmd_bits = 8, .addr_bits = 8, .dummy = 8, .lines = 4 };
        int a = spi_slave_hd_dma_rx(3, r, N);
        if (t == tx) {                                                       /* PSRAM TX: refused, not garbage */
            int e = spi_hd_xfer(2, &w, t, NULL, 4096);
            printf("%s SPI TX DMA from PSRAM refused (%d)\n", e < 0 ? "PASS" : "FAIL", e);
            all &= e < 0;
            continue;
        }
        for (int o = 0; o < N; o += 4096) spi_hd_xfer(2, &w, t + o, NULL, 4096);
        w.cmd = SPI_HD_WR_END | SPI_HD_QUAD; spi_hd_xfer(2, &w, NULL, NULL, 0);
        int n = spi_slave_hd_dma_rx_done(3), bad = 0;
        for (int i = 0; i < N; i++) bad += r[i] != (uint8_t)(i * 7 + 3 + cs);
        static const char *nm[] = { "PSRAM -> SPI -> internal", "internal -> SPI -> PSRAM", "PSRAM -> SPI -> PSRAM" };
        printf("%s %s: arm %d, %d B, %d wrong (first %02x)\n", a == 0 && n == N && !bad ? "PASS" : "FAIL", nm[cs], a, n, bad, r[0]);
        all &= a == 0 && n == N && !bad;
    }
    int a = 0, n = N, bad = !all;
    /* RGB LCD scanning a PSRAM frame buffer into the camera (DVP loop as periph_test cam, pads 16-41, no jumpers) */
    {
        static const int8_t pads[8] = { 16, 17, 18, 21, 38, 39, 40, 41 };
        uint8_t *fb = (uint8_t *)PSRAM_VADDR + 0x20000; static uint8_t got[512] __attribute__((aligned(64)));
        for (int i = 0; i < 256; i++) fb[i] = (uint8_t)(i * 13 + 7);              /* dirty in the cache */
        lcd_rgb_cfg_t rc = { .pclk = 14, .hsync = -1, .vsync = 4, .de = 5, .bits = 8, .pclk_hz = 1000000,
                             .width = 32, .height = 8, .hsw = 4, .hbp = 4, .hfp = 4, .vsw = 2, .vbp = 2, .vfp = 2 };
        memcpy(rc.data, pads, 8);
        uint32_t ok = lcd_rgb_start(&rc, fb);
        cam_cfg_t cc = { .pclk = 14, .vsync = 4, .de = 5, .xclk = -1, .bits = 8 };
        memcpy(cc.data, pads, 8);
        cam_init(&cc);
        cam_capture(got, sizeof got); cam_wait(100000);
        cam_capture(got, sizeof got); int n2 = cam_wait(100000);
        lcd_cam_release();
        int b2 = 0; for (int i = 0; i < 256; i++) b2 += got[i] != fb[i];
        printf("%s RGB LCD from a PSRAM frame buffer -> camera: start %lu, %d B, %d of 256 wrong\n", ok && n2 >= 256 && !b2 ? "PASS" : (ok ? "FAIL" : "REFUSED"), (unsigned long)ok, n2, b2);
        all &= (ok && n2 >= 256 && !b2) || !ok;
    }
    /* UART DMA TX from PSRAM, 1 Mbaud over the 12 -> 13 jumper */
    {
        uint8_t *ut = (uint8_t *)PSRAM_VADDR + 0x30000; static uint8_t ur[2048] __attribute__((aligned(64)));
        for (int i = 0; i < 2048; i++) ut[i] = (uint8_t)(i * 5 + 1);
        uart_cfg_t uc = { .baud = 1000000, .tx_pin = 12, .rx_pin = 13 };
        uart_init(1, &uc); uart_dma_start(1, 0);
        memset(ur, 0, sizeof ur);
        uart_dma_read_start(1, ur, sizeof ur);
        int w = uart_dma_write(1, ut, 2048, 100000);
        int got = -1; uint32_t t9 = ccount(); while ((got = uart_dma_read_done(1)) < 0 && ccount() - t9 < 240000000u) {}
        int b3 = 0; for (int i = 0; i < 2048; i++) b3 += ur[i] != (uint8_t)(i * 5 + 1);
        uart_dma_stop(1);
        printf("%s UART DMA TX from PSRAM: write %d, %d B, %d wrong\n", w > 0 && got == 2048 && !b3 ? "PASS" : w < 0 ? "REFUSED" : "FAIL", w, got, b3);
        all &= (w > 0 && got == 2048 && !b3) || w < 0;
    }
    bad = !all;
    gdma_desc_t d[2];
    int r = gdma_chain(d, 2, tx + 4, 100, true);
    printf("%s unaligned PSRAM buffer refused (%d)\n", r < 0 ? "PASS" : "FAIL", r);
    printf("dma_psram_test %s done\n", a == 0 && n == N && !bad && r < 0 ? "PASS" : "FAIL");
    for (;;) {}
}
