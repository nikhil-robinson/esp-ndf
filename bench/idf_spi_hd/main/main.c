/* IDF sibling of examples/periph_test/test_spi_hd.c: same pads and cross-routing, SPI2 master HD quad,
 * SPI3 spi_slave_hd (shared buffer registers). */
#include <stdio.h>
#include <string.h>
#include "driver/spi_master.h"
#include "driver/spi_slave_hd.h"
#include "esp_rom_gpio.h"
#include "esp_cpu.h"

static const int mpad[4] = { 16, 17, 18, 21 }, spad[4] = { 1, 2, 42, 47 };
static const int s2_io[4] = { 103, 102, 105, 104 }, s3_io[4] = { 68, 67, 70, 69 };

void app_main(void)
{
    spi_bus_config_t sb = { .sclk_io_num = 14, .data0_io_num = spad[0], .data1_io_num = spad[1],
                            .data2_io_num = spad[2], .data3_io_num = spad[3], .data4_io_num = -1, .data5_io_num = -1,
                            .data6_io_num = -1, .data7_io_num = -1, .max_transfer_sz = 4096 };
    spi_slave_hd_slot_config_t sc = { .spics_io_num = 15, .command_bits = 8, .address_bits = 8, .dummy_bits = 8,
                                      .queue_size = 2, .dma_chan = SPI_DMA_CH_AUTO, .mode = 0 };
    printf("slave init %d\n", spi_slave_hd_init(SPI3_HOST, &sb, &sc));
    spi_bus_config_t mb = { .sclk_io_num = 14, .data0_io_num = mpad[0], .data1_io_num = mpad[1],
                            .data2_io_num = mpad[2], .data3_io_num = mpad[3], .data4_io_num = -1, .data5_io_num = -1,
                            .data6_io_num = -1, .data7_io_num = -1, .max_transfer_sz = 4096, .flags = SPICOMMON_BUSFLAG_QUAD };
    spi_bus_initialize(SPI2_HOST, &mb, SPI_DMA_CH_AUTO);
    spi_device_interface_config_t dc = { .command_bits = 8, .address_bits = 8, .dummy_bits = 8, .clock_speed_hz = 10000000,
                                         .mode = 0, .spics_io_num = 15, .queue_size = 2, .flags = SPI_DEVICE_HALFDUPLEX };
    spi_device_handle_t d;
    spi_bus_add_device(SPI2_HOST, &dc, &d);
    for (int i = 0; i < 4; i++) {                              /* cross the inputs */
        esp_rom_gpio_connect_in_signal(spad[i], s2_io[i], false);
        esp_rom_gpio_connect_in_signal(mpad[i], s3_io[i], false);
    }
    esp_rom_gpio_connect_in_signal(14, 66, false);
    esp_rom_gpio_connect_in_signal(15, 71, false);
    static uint8_t out[32], got[32];
    for (int i = 0; i < 32; i++) out[i] = i * 37 + 3;
    spi_transaction_t t = { .flags = SPI_TRANS_MODE_QIO, .cmd = 0x01 | 0x20, .addr = 0, .length = 256, .tx_buffer = out };
    spi_device_polling_transmit(d, &t);
    uint32_t best = ~0u;
    for (int k = 0; k < 8; k++) {
        uint32_t c = esp_cpu_get_cycle_count();
        spi_device_polling_transmit(d, &t);
        c = esp_cpu_get_cycle_count() - c;
        if (c < best) best = c;
    }
    spi_slave_hd_read_buffer(SPI3_HOST, 0, got, 32);
    printf("quad write 32 B: %s, best %lu cycles\n", memcmp(out, got, 32) ? "BAD" : "ok", best);
    /* DMA segment mode, as periph_test hd_dma(): WRDMA 3 seg + WR_END, RDDMA 2 seg + INT0, 1 and 4 lines */
    static uint8_t dm[3096] __attribute__((aligned(4))), ds[3096] __attribute__((aligned(4)));
    for (int q = 0; q < 2; q++) {
        uint32_t fl = q ? SPI_TRANS_MODE_QIO : 0, mod = q ? 0x20 : 0;
        static const int ws[3] = { 1000, 2000, 96 }, rs[2] = { 2048, 1048 };
        for (int i = 0; i < 3096; i++) dm[i] = i * 13 + (q ? 4 : 1);
        memset(ds, 0, sizeof ds);
        spi_slave_hd_data_t sd = { .data = ds, .len = 3096 }, *res;
        spi_slave_hd_queue_trans(SPI3_HOST, SPI_SLAVE_CHAN_RX, &sd, portMAX_DELAY);
        uint32_t c = esp_cpu_get_cycle_count();
        for (int k = 0, o = 0; k < 3; o += ws[k++]) {
            spi_transaction_ext_t e = { .base = { .flags = fl, .cmd = 0x03 | mod, .length = ws[k] * 8, .tx_buffer = dm + o } };
            spi_device_polling_transmit(d, &e.base);
        }
        c = esp_cpu_get_cycle_count() - c;
        spi_transaction_t end = { .flags = fl, .cmd = 0x07 | mod };
        spi_device_polling_transmit(d, &end);
        spi_slave_hd_get_trans_res(SPI3_HOST, SPI_SLAVE_CHAN_RX, &res, portMAX_DELAY);
        bool wok = res->trans_len == 3096 && !memcmp(dm, ds, 3096);
        for (int i = 0; i < 3096; i++) ds[i] = dm[i] ^ 0xA5;
        memset(dm, 0, sizeof dm);
        spi_slave_hd_queue_trans(SPI3_HOST, SPI_SLAVE_CHAN_TX, &sd, portMAX_DELAY);
        for (int k = 0, o = 0; k < 2; o += rs[k++]) {
            spi_transaction_t r = { .flags = fl, .cmd = 0x04 | mod, .rxlength = rs[k] * 8, .rx_buffer = dm + o };
            spi_device_polling_transmit(d, &r);
        }
        spi_transaction_t i0 = { .flags = fl, .cmd = 0x08 | mod };
        spi_device_polling_transmit(d, &i0);
        spi_slave_hd_get_trans_res(SPI3_HOST, SPI_SLAVE_CHAN_TX, &res, portMAX_DELAY);
        printf("DMA %d-line: WRDMA 3 seg -> %d B %s (%lu us), RDDMA 2 seg %s\n", q ? 4 : 1, (int)sd.trans_len,
               wok ? "ok" : "BAD", c / 240, memcmp(dm, ds, 3096) ? "BAD" : "ok");
    }
    printf("idf_spi_hd done\n");
}
