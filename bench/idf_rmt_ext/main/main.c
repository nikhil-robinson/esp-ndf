/* IDF sibling of the WS2812 part of components/drivers/*/test_apps/*_unit/test_rmt_ext.c: TX GPIO10, 10 MHz, 3 LEDs. */
#include <stdio.h>
#include "driver/rmt_tx.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    rmt_channel_handle_t ch;
    rmt_tx_channel_config_t tc = { .gpio_num = 10, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000,
                                   .mem_block_symbols = 48, .trans_queue_depth = 4 };
    rmt_new_tx_channel(&tc, &ch);
    rmt_bytes_encoder_config_t bc = {
        .bit0 = { .level0 = 1, .duration0 = 4, .level1 = 0, .duration1 = 8 },
        .bit1 = { .level0 = 1, .duration0 = 8, .level1 = 0, .duration1 = 4 }, .flags.msb_first = 1 };
    rmt_encoder_handle_t enc;
    rmt_new_bytes_encoder(&bc, &enc);
    rmt_enable(ch);
    const uint8_t grb[9] = { 0x10, 0, 0, 0, 0x10, 0, 0xA5, 0x5A, 0xFF };
    rmt_transmit_config_t t = { .loop_count = 0 };
    uint32_t bcall = ~0u, bdone = ~0u;
    for (int k = 0; k < 8; k++) {
        uint32_t c = esp_cpu_get_cycle_count();
        rmt_transmit(ch, enc, grb, 9, &t);
        uint32_t c1 = esp_cpu_get_cycle_count();
        rmt_tx_wait_all_done(ch, 100);
        uint32_t c2 = esp_cpu_get_cycle_count();
        if (c1 - c < bcall) bcall = c1 - c;
        if (c2 - c < bdone) bdone = c2 - c;
        vTaskDelay(1);
    }
    printf("WS2812 3 LEDs: rmt_transmit %lu cycles, until done %lu cycles\n", bcall, bdone);
    printf("idf_rmt_ext done\n");
}
