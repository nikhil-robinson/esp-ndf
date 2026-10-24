/* IDF sibling of examples/periph_test/test_i2s_modes.c, same jumpers (12->13 clock, 10->11 data, WS 4). */
#include <stdio.h>
#include <math.h>
#include "driver/i2s_tdm.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_cpu.h"

static int16_t out[512], in[2048];

static void tdm(void)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&cc, &tx, NULL);
    i2s_chan_config_t rc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    i2s_new_channel(&rc, NULL, &rx);
    i2s_tdm_config_t c = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT3),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = 12, .ws = 4, .dout = 10, .din = I2S_GPIO_UNUSED },
    };
    c.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
    c.slot_cfg.total_slot = 4;
    uint32_t t = esp_cpu_get_cycle_count();
    i2s_channel_init_tdm_mode(tx, &c);
    t = esp_cpu_get_cycle_count() - t;
    c.slot_cfg.slot_mask = 0xF;
    c.gpio_cfg = (i2s_tdm_gpio_config_t){ .mclk = I2S_GPIO_UNUSED, .bclk = 13, .ws = 4, .dout = I2S_GPIO_UNUSED, .din = 11 };
    i2s_channel_init_tdm_mode(rx, &c);
    i2s_channel_enable(rx); i2s_channel_enable(tx);
    uint16_t v = 1, prev = 0; uint32_t got = 0, seen = 0, zeros = 0, bad = 0; size_t w, r;
    while (got < 64 * 1024) {
        for (int i = 0; i < 512; i++) out[i] = v++;
        i2s_channel_write(tx, out, sizeof out, &w, 100);
        r = 0; i2s_channel_read(rx, in, sizeof in, &r, 0);
        for (int i = 0; i < (int)r / 2; i++) {
            uint16_t x = in[i];
            if (!seen && !x) continue;
            if (!x) { zeros++; continue; }
            if (seen && x != (uint16_t)(prev + 1)) bad++;
            prev = x; seen++;
        }
        got += r;
    }
    printf("    TDM: %lu samples, %lu masked zeros, %lu errors, tx init %lu cycles\n", seen, zeros, bad, t);
    i2s_channel_disable(tx); i2s_channel_disable(rx); i2s_del_channel(tx); i2s_del_channel(rx);
}

static void pdm(void)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&cc, &tx, &rx);
    i2s_pdm_tx_config_t tc = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .clk = 12, .dout = 10 },
    };
    uint32_t t = esp_cpu_get_cycle_count();
    i2s_channel_init_pdm_tx_mode(tx, &tc);
    t = esp_cpu_get_cycle_count() - t;
    i2s_pdm_rx_config_t rc = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .clk = 12, .din = 11 },
    };
    rc.clk_cfg.dn_sample_mode = I2S_PDM_DSR_16S;
    printf("    PDM rx init: %d\n", i2s_channel_init_pdm_rx_mode(rx, &rc));
    i2s_channel_enable(rx); i2s_channel_enable(tx);
    uint32_t ph = 0, got = 0, n_s = 0, cross = 0; size_t w, r;
    int16_t lo = 0, hi = 0; int neg = 0;
    float q1 = 0, q2 = 0, pw = 0, cf = 2 * cosf(6.2831853f / 48);
    while (n_s < 24000) {
        for (int i = 0; i < 512; i += 2, ph = (ph + 1) % 48) out[i] = out[i + 1] = (int16_t)(12000 * sinf(ph * (6.2831853f / 48)));
        i2s_channel_write(tx, out, sizeof out, &w, 100);
        r = 0; i2s_channel_read(rx, in, sizeof in, &r, 0);
        for (int i = 0; i < (int)r / 2; i += 2, got++) {
            if (got < 4800) continue;
            int16_t x = in[i];
            if (x < lo) lo = x;
            if (x > hi) hi = x;
            if (x < -3000) neg = 1; else if (neg && x > 3000) { neg = 0; cross++; }
            float q0 = cf * q1 - q2 + x; q2 = q1; q1 = q0; pw += (float)x * x;
            n_s++;
        }
    }
    float tone = (q1 * q1 + q2 * q2 - cf * q1 * q2) * 2 / n_s;
    printf("    PDM loop: %lu samples, %lu Hz, peak %d..%d, SNR %d dB, tx init %lu cycles\n", n_s,
           (uint32_t)((uint64_t)cross * 48000 / n_s), lo, hi, (int)(10 * log10f(tone / (pw - tone))), t);
    i2s_channel_disable(tx); i2s_channel_disable(rx); i2s_del_channel(tx); i2s_del_channel(rx);
}

void app_main(void)
{
    for (int p = 4; p <= 13; p++) if (p < 7 || p > 9) gpio_reset_pin(p);
    tdm();
    pdm();
    printf("idf_i2s_modes done\n");
}
