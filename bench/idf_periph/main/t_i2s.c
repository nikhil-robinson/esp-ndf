#include "t.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* same as esp-ndf test: I2S0 TX master BCK12/WS4/DOUT10 -> jumpers -> I2S1 RX slave BCK13/WS4/DIN11 */
static uint16_t out[512], in[2048];
void t_i2s(void)
{
    for (int p = 4; p <= 13; p++) if (p < 7 || p > 9) gpio_reset_pin(p);
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&cc, &tx, NULL);
    i2s_chan_config_t rc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    i2s_new_channel(&rc, NULL, &rx);
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = 12, .ws = 4, .dout = 10, .din = I2S_GPIO_UNUSED },
    };
    sc.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
    i2s_channel_init_std_mode(tx, &sc);
    sc.gpio_cfg = (i2s_std_gpio_config_t){ .mclk = I2S_GPIO_UNUSED, .bclk = 13, .ws = 4, .dout = I2S_GPIO_UNUSED, .din = 11 };
    i2s_channel_init_std_mode(rx, &sc);
    i2s_channel_enable(rx); i2s_channel_enable(tx);
    uint16_t v = 1, prev = 0; uint32_t got = 0, seen = 0, breaks = 0; size_t w, r;
    while (got < 64 * 1024) {
        for (int i = 0; i < 512; i++) out[i] = v++;
        i2s_channel_write(tx, out, sizeof out, &w, 100);
        r = 0; i2s_channel_read(rx, in, sizeof in, &r, 0);
        for (int i = 0; i < (int)r / 2; i++) { if (in[i] == 0) continue; if (seen && in[i] != (uint16_t)(prev + 1)) breaks++; prev = in[i]; seen++; }
        got += r;
    }
    printf("    received %lu bytes, %lu samples checked, %lu discontinuities\n", got, seen, breaks);
    i2s_channel_disable(tx); i2s_channel_disable(rx); i2s_del_channel(tx); i2s_del_channel(rx);
}
