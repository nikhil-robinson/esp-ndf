#include "t.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#define N 100
static rmt_symbol_word_t out[N], in[N + 4];
static volatile int rx_n = -1;
static bool IRAM_ATTR rx_done(rmt_channel_handle_t c, const rmt_rx_done_event_data_t *e, void *a) { rx_n = e->num_symbols; return false; }
void t_rmt(void)
{
    rmt_channel_handle_t tx, rx;
    rmt_tx_channel_config_t tc = { .gpio_num = 10, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000, .mem_block_symbols = 48, .trans_queue_depth = 4 };
    rmt_new_tx_channel(&tc, &tx);
    rmt_rx_channel_config_t rc = { .gpio_num = 11, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000, .mem_block_symbols = 48 };
    rmt_new_rx_channel(&rc, &rx);
    rmt_rx_event_callbacks_t cb = { .on_recv_done = rx_done };
    rmt_rx_register_event_callbacks(rx, &cb, 0);
    rmt_encoder_handle_t enc; rmt_copy_encoder_config_t ec = {}; rmt_new_copy_encoder(&ec, &enc);
    rmt_enable(tx); rmt_enable(rx);
    for (int i = 0; i < N; i++) out[i] = (rmt_symbol_word_t){ .level0 = 1, .duration0 = 10 + i, .level1 = 0, .duration1 = 20 + 2 * i };
    rmt_receive_config_t rcv = { .signal_range_min_ns = 100, .signal_range_max_ns = 200000 };
    rmt_receive(rx, in, sizeof in, &rcv);
    rmt_transmit_config_t txc = { .loop_count = 0 };
    uint32_t t0 = ccount();
    rmt_transmit(tx, enc, out, sizeof out, &txc);
    uint32_t start = ccount() - t0;
    rmt_tx_wait_all_done(tx, 1000);
    for (int k = 0; k < 100 && rx_n < 0; k++) esp_rom_delay_us(1000);
    int worst = 0;
    for (int i = 0; i < N - 1 && i < rx_n; i++) {
        int e0 = abs((int)in[i].duration0 - (int)out[i].duration0), e1 = abs((int)in[i].duration1 - (int)out[i].duration1);
        if (e0 > worst) worst = e0;
        if (e1 > worst) worst = e1;
    }
    METRIC("TX 100 items / RX received", "%d items", rx_n);
    METRIC("worst duration error", "%d ticks (100 ns)", worst);
    METRIC("rmt_tx() start (100 items)", "%lu cycles", start);
}
