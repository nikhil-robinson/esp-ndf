#include "test.h"
#include "lcd_i80.h"
#include "pcnt.h"
#include "gpio.h"
#include "sys.h"
#include <string.h>

/* i80 8-bit: D0-D7 on 16 17 18 21 38 39 40 41, WR 14, DC 15; core 1 records (DC, byte) at each WR rise.
 * RGB 8-bit timing on the same pads: PCNT counts PCLK / HSYNC per VSYNC. */
#define GPIO_IN 0x6000403C
#define GPIO_IN1 0x60004040
static const int8_t pads[8] = { 16, 17, 18, 21, 38, 39, 40, 41 };
static volatile uint32_t cap_go, cap_n; static uint16_t cap[512];

static void capture(void)
{
    for (;;) {
        while (!cap_go) {}
        uint32_t prev = REG(GPIO_IN), t0 = ccount();
        while (cap_n < 512 && ccount() - t0 < 120000000u) {
            uint32_t lo = REG(GPIO_IN), hi = REG(GPIO_IN1);
            if ((lo & BIT(14)) && !(prev & BIT(14))) {        /* WR rising: the i80 latch edge */
                uint32_t b = (lo >> 16 & 7) | (lo >> 21 & 1) << 3 | (hi >> 6 & 0xF) << 4;   /* 38..41 = in1 bits 6..9 */
                cap[cap_n++] = b | (lo >> 15 & 1) << 8;
            }
            prev = lo;
        }
        cap_go = 0;
    }
}

static uint8_t px[300] __attribute__((aligned(4)));

void test_lcd_i80(void)
{
    __asm__ volatile("rsil a2, 0" ::: "a2");
    lcd_i80_cfg_t c = { .wr = 14, .dc = 15, .cs = -1, .bits = 8, .pclk_hz = 25000 };
    memcpy(c.data, pads, 8);
    uint32_t hz = lcd_i80_init(&c);
    printf("    i80 8-bit WR clock %lu Hz (asked 25 kHz)\n", hz);
    CHECK(hz > 24000 && hz < 26000);
    for (int i = 0; i < 300; i++) px[i] = (uint8_t)(i * 7 + 3);
    cap_n = 0;
    cpu1_start(capture);
    cap_go = 1;
    static uint8_t par[4] __attribute__((aligned(4))) = { 0x00, 0x10, 0x01, 0x3F };
    delay_us(1000);                                          /* core 1 in its loop */
    CHECK(lcd_i80_send(0x2A, par, 4) == 0);
    CHECK(lcd_i80_send(0x2C, px, 300) == 0);
    uint32_t t0 = ccount(); while (cap_n < 306 && ccount() - t0 < 240000000u) {}
    cap_go = 0;
    int bad = 0;
    bad += cap[0] != 0x02A;                                 /* DC 0 = command */
    for (int i = 0; i < 4; i++) bad += cap[1 + i] != (0x100 | par[i]);
    bad += cap[5] != 0x02C;
    for (int i = 0; i < 300; i++) bad += cap[6 + i] != (0x100 | px[i]);
    printf("    cmd 2A + 4 params, cmd 2C + 300 bytes: %lu WR strobes captured, %d wrong\n", cap_n, bad);
    CHECK(cap_n == 306 && bad == 0);
    METRIC("lcd_i80_send_async() 300 B start", "%lu cycles", ({ uint32_t t = ccount(); lcd_i80_send_async(0x2C, px, 300); uint32_t r = ccount() - t; lcd_i80_wait(); r; }));

    /* RGB: 32 x 8 frame, hsw 4 hbp 4 hfp 4, vsw 2 vbp 2 vfp 2 at 1 MHz: 44 pclk per line, 14 lines */
    static uint8_t fb[32 * 8] __attribute__((aligned(4)));
    lcd_rgb_cfg_t r = { .pclk = 14, .hsync = 15, .vsync = 4, .de = -1, .bits = 8, .pclk_hz = 1000000,
                        .width = 32, .height = 8, .hsw = 4, .hbp = 4, .hfp = 4, .vsw = 2, .vbp = 2, .vfp = 2 };
    memcpy(r.data, pads, 8);
    pcnt_channel(2, 0, 14, -1, PCNT_EDGE_UP, PCNT_EDGE_OFF, PCNT_CTRL_KEEP, PCNT_CTRL_KEEP);   /* pclk */
    pcnt_channel(3, 0, 15, -1, PCNT_EDGE_OFF, PCNT_EDGE_UP, PCNT_CTRL_KEEP, PCNT_CTRL_KEEP);   /* hsync (active low) */
    uint32_t rhz = lcd_rgb_start(&r, fb);
    delay_us(20000);
    pcnt_clear(2); pcnt_clear(3);
    uint32_t f0 = lcd_rgb_frames();
    delay_us(100000);
    int32_t pc = pcnt_get(2), hs = pcnt_get(3);
    uint32_t fr = lcd_rgb_frames() - f0;
    lcd_cam_release();
    printf("    RGB 32x8 @%lu Hz: %lu frames in 100 ms, %ld pclk, %ld hsync -> %ld pclk/line, %ld lines/frame (44, 14)\n",
           rhz, fr, pc, hs, hs ? pc / hs : 0, fr ? (hs + (int32_t)fr / 2) / (int32_t)fr : 0);
    CHECK(fr >= 160 && fr <= 166 && hs && pc / hs == 44 && (hs + (int32_t)fr / 2) / (int32_t)fr == 14);   /* window edges: round */
}

/* DVP loop: the LCD side streams a 32x8 RGB frame (PCLK 14, VSYNC 4, DE 5, D0-D7 as above), the camera
 * side captures it from the same pads by DMA. Second frame must equal the frame buffer. */
#include "cam.h"
void test_cam(void)
{
    __asm__ volatile("rsil a2, 0" ::: "a2");
    static uint8_t fb[32 * 8] __attribute__((aligned(4))), got[512] __attribute__((aligned(4)));
    for (int i = 0; i < 256; i++) fb[i] = (uint8_t)(i * 13 + 7);
    lcd_rgb_cfg_t r = { .pclk = 14, .hsync = -1, .vsync = 4, .de = 5, .bits = 8, .pclk_hz = 1000000,
                        .width = 32, .height = 8, .hsw = 4, .hbp = 4, .hfp = 4, .vsw = 2, .vbp = 2, .vfp = 2 };
    memcpy(r.data, pads, 8);
    lcd_rgb_start(&r, fb);
    cam_cfg_t c = { .pclk = 14, .vsync = 4, .de = 5, .xclk = -1, .bits = 8 };
    memcpy(c.data, pads, 8);
    CHECK(cam_init(&c) == 0);
    int n1 = (cam_capture(got, sizeof got), cam_wait(100000));    /* first may be partial */
    int n = (cam_capture(got, sizeof got), cam_wait(100000));
    lcd_cam_release();
    int bad = 0, off = 0;
    for (int i = 0; i < 256 && i < n; i++) bad += got[i] != fb[i];
    if (bad) for (off = 1; off < 4; off++) { int b2 = 0; for (int i = 0; i + off < n && i < 250; i++) b2 += got[i + off] != fb[i]; if (!b2) break; }
    printf("    DVP loop 32x8: frames of %d / %d bytes, %d of 256 wrong%s\n", n1, n, bad, bad && off < 4 ? " (shifted)" : "");
    if (bad) { for (int i = 0; i < 8; i++) printf(" %02x/%02x", got[i], fb[i]); printf("\n"); }
    CHECK(n >= 256 && bad == 0);
}
