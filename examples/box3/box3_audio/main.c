/* BOX-3 speaker: ES8311 codec (I2C 0x18) fed by I2S0 as master, MCLK = 256 fs.
 * Pins (esp-bsp esp-box-3.h): I2C SCL18 SDA8, MCLK2 BCK17 WS45 DOUT15, PA enable 46. */
#include "i2c.h"
#include "i2s.h"
#include "gpio.h"
#include "console.h"
#include "sys.h"

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

/* 48-step sine, amplitude 6000 */
static const int16_t sine48[48] = {
    0, 783, 1553, 2296, 3000, 3653, 4243, 4760, 5196, 5543, 5796, 5949, 6000, 5949, 5796, 5543,
    5196, 4760, 4243, 3653, 3000, 2296, 1553, 783, 0, -783, -1553, -2296, -3000, -3653, -4243, -4760,
    -5196, -5543, -5796, -5949, -6000, -5949, -5796, -5543, -5196, -4760, -4243, -3653, -3000, -2296, -1553, -783 };
static int16_t buf[512];

static void tone(uint32_t hz, uint32_t ms)
{
    uint32_t phase = 0, step = hz * 48 * 65536 / 48000;   /* 16.16 phase in sine48 steps */
    uint32_t frames = 48000 * ms / 1000;
    while (frames) {
        int n = frames > 256 ? 256 : frames;
        for (int i = 0; i < n; i++) {
            int16_t s = sine48[(phase >> 16) % 48];
            buf[2 * i] = s; buf[2 * i + 1] = s;              /* L, R */
            phase += step;
        }
        i2s_write(0, buf, n * 4, 100000);
        frames -= n;
    }
}

int main(void)
{
    printf("\nbox3_audio: init\n");
    i2c_master_init(0, 8, 18, 400000, false);
    i2s_cfg_t c = { .rate = 48000, .bits = 16, .mclk = 2, .bck = 17, .ws = 45, .dout = 15, .din = -1 };
    uint64_t fs = i2s_init(0, &c);
    i2s_start(0, true, false);                             /* MCLK running before the codec powers up */
    delay_us(10000);
    es8311_init();
    gpio_config(46, GPIO_OUTPUT); gpio_set(46);            /* power amplifier on */
    printf("box3_audio: fs %lu.%03lu Hz, playing 1 kHz for 2 s, then a chime\n", (uint32_t)(fs / 1000), (uint32_t)(fs % 1000));
    tone(1000, 2000);
    tone(0, 300);
    tone(784, 250); tone(988, 250); tone(1175, 500);
    tone(0, 200);
    gpio_clr(46);
    printf("box3_audio: done, tx underruns %lu\n", i2s_tx_underruns(0));
    return 0;
}
