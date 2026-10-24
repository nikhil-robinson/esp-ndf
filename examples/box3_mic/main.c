/* BOX-3 microphones end to end: ES8311 plays 1 kHz through the speaker while the ES7210 (I2C 0x40) records
 * MIC1/MIC2 on I2S0 RX (full duplex with the speaker TX: MCLK 2, BCK 17, WS 45, DOUT 15, DIN 16).
 * ES7210 sequence from the Linux es7210.c (normal I2S, 16 bit, MCLK 256 fs). Goertzel at 1 kHz per mic,
 * tone on vs tone off. */
#include "i2c.h"
#include "i2s.h"
#include "gpio.h"
#include "console.h"
#include "sys.h"
#include "fastmath.h"

static void wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    i2c_master_xfer(0, 0x18, b, 2, 0, 0, 20000);
}

static void es8311_init(void)
{
    wr(0xFA, 0x01); delay_us(20000); wr(0xFA, 0x00);      /* reset */
    wr(0x00, 0x1F); wr(0x45, 0x00);
    wr(0x01, 0x30); wr(0x02, 0x00); wr(0x03, 0x10); wr(0x04, 0x10); wr(0x05, 0x00);   /* clocks: MCLK = 256 fs */
    wr(0x0B, 0x00); wr(0x0C, 0x00); wr(0x10, 0x1F); wr(0x11, 0x7F);
    wr(0x00, 0x80); delay_us(20000);                       /* power up, slave */
    wr(0x0D, 0x01);                                        /* analog on */
    wr(0x01, 0x3F);                                        /* digital clocks on */
    wr(0x09, 0x0C); wr(0x0A, 0x0C);                        /* SDP in/out: I2S, 16-bit */
    wr(0x0E, 0x02); wr(0x13, 0x10); wr(0x1C, 0x6A); wr(0x37, 0x08);
    wr(0x12, 0x00);                                        /* DAC on */
    wr(0x31, 0x00);                                        /* unmute */
    wr(0x32, 0xA8);                                        /* DAC volume ~ -12 dB */
}


static void es7210(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    i2c_master_xfer(0, 0x40, b, 2, 0, 0, 20000);
}

static void es7210_init(void)
{
    static const uint8_t seq[][2] = {
        { 0x00, 0xFF }, { 0x00, 0x32 }, { 0x09, 0x30 }, { 0x0A, 0x30 },                      /* reset, timing */
        { 0x23, 0x2A }, { 0x22, 0x0A }, { 0x21, 0x2A }, { 0x20, 0x0A },                      /* high-pass */
        { 0x11, 0x60 }, { 0x12, 0x00 },                                                      /* I2S 16 bit, normal */
        { 0x40, 0xC3 }, { 0x41, 0x70 }, { 0x42, 0x70 },                                      /* analog, mic bias */
        { 0x43, 0x1A }, { 0x44, 0x1A }, { 0x45, 0x1A }, { 0x46, 0x1A },                      /* mic gain */
        { 0x47, 0x08 }, { 0x48, 0x08 }, { 0x49, 0x08 }, { 0x4A, 0x08 }, { 0x07, 0x20 },
        { 0x02, 0xC1 },                                                                      /* MCLK = 256 fs */
        { 0x06, 0x00 }, { 0x4B, 0x0F }, { 0x4C, 0x0F }, { 0x00, 0x71 }, { 0x00, 0x41 },      /* power up */
        { 0x14, 0x00 }, { 0x15, 0x00 },                                                      /* unmute */
    };
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) es7210(seq[i][0], seq[i][1]);
}

/* 48-step sine, amplitude 6000 */
static const int16_t sine48[48] = {
    0, 783, 1553, 2296, 3000, 3653, 4243, 4760, 5196, 5543, 5796, 5949, 6000, 5949, 5796, 5543,
    5196, 4760, 4243, 3653, 3000, 2296, 1553, 783, 0, -783, -1553, -2296, -3000, -3653, -4243, -4760,
    -5196, -5543, -5796, -5949, -6000, -5949, -5796, -5543, -5196, -4760, -4243, -3653, -3000, -2296, -1553, -783 };

static int16_t tx[512], rx[1024];

/* play (amp != 0) or silence for ms, while measuring the 1 kHz share of each mic channel */
static void run(int on, uint32_t ms, int *snr_db, int *rms)
{
    uint32_t phase = 0, frames = 48000 * ms / 1000, got = 0;
    float q1[2] = { 0 }, q2[2] = { 0 }, pw[2] = { 0 }, cf = 2 * fm_cosf(6.2831853f * 1000 / 48000);
    while (frames) {
        for (int i = 0; i < 256; i++) { int16_t s = on ? sine48[(phase >> 16) % 48] : 0; tx[2 * i] = tx[2 * i + 1] = s; phase += 65536; }
        i2s_write(0, tx, sizeof tx, 100000);
        frames = frames > 256 ? frames - 256 : 0;
        int n = i2s_read(0, rx, sizeof rx, 0) / 4;
        for (int i = 0; i < n; i++, got++) {
            if (got < 4800) continue;                                   /* 100 ms settle */
            for (int c = 0; c < 2; c++) {
                float x = rx[2 * i + c];
                float q0 = cf * q1[c] - q2[c] + x; q2[c] = q1[c]; q1[c] = q0; pw[c] += x * x;
            }
        }
    }
    uint32_t n = got > 4800 ? got - 4800 : 1;
    for (int c = 0; c < 2; c++) {
        float tone = (q1[c] * q1[c] + q2[c] * q2[c] - cf * q1[c] * q2[c]) * 2 / n;
        snr_db[c] = pw[c] > tone && tone > 0 ? (int)(4.3429448f * fm_logf(tone / (pw[c] - tone))) : -99;
        rms[c] = (int)fm_sqrtf(pw[c] / n);
    }
}

int main(void)
{
    i2c_master_init(0, 8, 18, 400000, false);
    i2s_cfg_t c = { .rate = 48000, .bits = 16, .mclk = 2, .bck = 17, .ws = 45, .dout = 15, .din = 16 };
    uint64_t fs = i2s_init(0, &c);
    i2s_start(0, true, true);
    delay_us(10000);
    es8311_init();
    es7210_init();
    gpio_config(46, GPIO_OUTPUT); gpio_set(46);
    printf("box3_mic: fs %lu Hz, full duplex\n", (uint32_t)(fs / 1000));
    int son[2], ron[2], soff[2], roff[2];
    run(0, 500, soff, roff);
    run(1, 1000, son, ron);
    gpio_clr(46);
    printf("box3_mic: speaker off: MIC1 rms %d 1kHz %d dB, MIC2 rms %d 1kHz %d dB\n", roff[0], soff[0], roff[1], soff[1]);
    printf("box3_mic: 1 kHz on:    MIC1 rms %d 1kHz %d dB, MIC2 rms %d 1kHz %d dB\n", ron[0], son[0], ron[1], son[1]);
    bool ok = son[0] > 10 && son[1] > 10 && son[0] > soff[0] + 15 && son[1] > soff[1] + 15;
    printf("box3_mic %s done (rx overruns %lu)\n", ok ? "PASS" : "FAIL", i2s_rx_overruns(0));
    for (;;) {}
}
