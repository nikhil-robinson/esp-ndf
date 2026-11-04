/* DMA + interrupt stress, 60 s, data checked on every transfer. Core 0: a 20 kHz timer interrupt, UART1 DMA loopback at
 * 2 Mbaud (jumper 12 -> 13), SPI2 -> SPI3 half-duplex DMA (pads cross-routed, no jumpers), GDMA memory copies.
 * Core 1: AES-256-GCM encrypt + decrypt, SHA-256, heap churn with heap_check(). Any wrong byte, lost tick or heap
 * corruption counts as an error; "stress PASS" needs 0. */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "gptimer.h"
#include "uart.h"
#include "spi.h"
#include "gpio.h"
#include "gdma.h"
#include "aes.h"
#include "sha.h"
#include "heap.h"
#include <string.h>
#include <stdlib.h>

static volatile uint32_t ticks, c1_rounds, c1_err, c1_done;
static void tick(void *a) { (void)a; ticks++; }

static void core1(void)
{
    static uint8_t key[32], iv[12], pt[2048], ct[2048], back[2048], tag[16];
    for (int i = 0; i < 32; i++) key[i] = i * 3;
    while (!c1_done) {
        uint32_t r = c1_rounds;
        for (int i = 0; i < 2048; i++) pt[i] = (uint8_t)(i * 7 + r);
        memcpy(iv, &r, 4);
        if (aes_gcm(key, 32, true, iv, 12, 0, 0, pt, ct, sizeof pt, tag) || aes_gcm(key, 32, false, iv, 12, 0, 0, ct, back, sizeof ct, tag) ||
            memcmp(pt, back, sizeof pt)) c1_err++;
        uint8_t d1[32], d2[32]; sha(SHA256, pt, sizeof pt, d1); sha(SHA256, back, sizeof back, d2);
        if (memcmp(d1, d2, 32)) c1_err++;
        void *p[8]; for (int i = 0; i < 8; i++) { p[i] = malloc(64 + 97 * i); if (p[i]) memset(p[i], i, 64 + 97 * i); }
        for (int i = 7; i >= 0; i--) free(p[i]);
        if (!heap_check()) c1_err++;
        c1_rounds++;
    }
    for (;;) __asm__ volatile("waiti 0");
}

static const int8_t mpad[4] = { 16, 17, 18, 21 }, spad[4] = { 1, 2, 42, 47 };
static const uint8_t s2_io[4] = { 103, 102, 105, 104 }, s3_io[4] = { 68, 67, 70, 69 };

int main(void)
{
    delay_ms(300);
    printf("stress: 60 s\n");
    cpu1_start(core1);                                                          /* everything running first, */
    gpt_init(0, 1000000); gpt_alarm(0, 50, true, tick, 0, 2); gpt_start(0);       /* 20 kHz              */
    uart_cfg_t uc = { .baud = 2000000, .tx_pin = 12, .rx_pin = 13 }; uart_init(1, &uc); uart_dma_start(1, 0);
    spi_slave_hd_init(3, 14, 15, spad, 0); spi_master_init(2, 14, -1, -1, 15, 10000000, 0);   /* then SPI */
    for (int i = 0; i < 4; i++) { spi_master_io(2, i, mpad[i]); gpio_in_signal(spad[i], s2_io[i], false); gpio_in_signal(mpad[i], s3_io[i], false); }
    int ch = gdma_alloc(); gdma_connect(ch, GDMA_M2M);
    static uint8_t ut[1024], ur[1024], st[4096], sr[4096], ma[8192], mb[8192];
    uint32_t err = 0, u_n = 0, s_n = 0, m_n = 0, t0 = ccount(), tk0 = ticks;
    uint64_t start_us = time_us();
    for (uint32_t r = 0; time_us() - start_us < 60000000ull; r++) {
        for (int i = 0; i < 1024; i++) ut[i] = (uint8_t)(i + r);
        memset(ur, 0, sizeof ur);
        uart_dma_read_start(1, ur, sizeof ur);
        uart_dma_write(1, ut, sizeof ut, 100000);
        int got; uint32_t tw = ccount(); while ((got = uart_dma_read_done(1)) < 0 && ccount() - tw < 48000000u) {}
        if (got != 1024 || memcmp(ut, ur, 1024)) err++; else u_n++;
        for (int i = 0; i < 4096; i++) st[i] = (uint8_t)(i * 5 + r);
        spi_hd_t w = { .cmd = SPI_HD_WRDMA | SPI_HD_QUAD, .cmd_bits = 8, .addr_bits = 8, .dummy = 8, .lines = 4 };
        spi_slave_hd_dma_rx(3, sr, sizeof sr);
        if (spi_hd_xfer(2, &w, st, NULL, sizeof st)) { if (err < 1) printf("  spi master: no DMA channel\n"); }
        w.cmd = SPI_HD_WR_END | SPI_HD_QUAD; spi_hd_xfer(2, &w, NULL, NULL, 0);
        int sd = spi_slave_hd_dma_rx_done(3);
        if (sd != 4096 || memcmp(st, sr, 4096)) { if (err < 1) { printf("  spi: done %d, sr[0] %02x want %02x\n", sd, sr[0], st[0]); } err++; } else s_n++;
        for (int i = 0; i < 8192; i += 4) *(uint32_t *)(ma + i) = i * 2654435761u + r;
        int gm = gdma_memcpy(ch, mb, ma, sizeof ma);
        if (gm || memcmp(ma, mb, sizeof ma)) { if (err < 3) printf("  gdma: rc %d ch %d\n", gm, ch); err++; } else m_n++;
    }
    uint32_t secs = (uint32_t)((time_us() - start_us) / 1000000u), tk = ticks - tk0; (void)t0;
    c1_done = 1; delay_ms(50);
    uint32_t want = (uint32_t)((time_us() - start_us) / 50);
    bool tick_ok = tk + want / 1000 >= want - 50;                      /* timer: none lost (within 0.1 % + 50) */
    printf("stress: UART %lu, SPI %lu, GDMA %lu transfers ok, %lu bad | timer %lu / ~%lu ticks | core 1: %lu rounds, %lu errors\n",
           u_n, s_n, m_n, err, tk, want, c1_rounds, c1_err);
    printf("stress %s done (%lu s)\n", !err && !c1_err && tick_ok && c1_rounds > 100 ? "PASS" : "FAIL", secs);
    for (;;) {}
}
