#include "t.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_periph.h"
#define LEN 4096
void t_spi(void)
{
    uint8_t *tx = heap_caps_malloc(LEN, MALLOC_CAP_DMA), *rx = heap_caps_malloc(LEN, MALLOC_CAP_DMA);
    for (int i = 0; i < LEN; i++) tx[i] = i * 13 + 7;
    for (int p = 4; p <= 13; p++) if (p < 7 || p > 9) gpio_reset_pin(p);   /* earlier siblings used these */
    spi_bus_config_t bc = { .mosi_io_num = 10, .miso_io_num = -1, .sclk_io_num = 6, .quadwp_io_num = -1,
                            .quadhd_io_num = -1, .max_transfer_sz = LEN };
    spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_CH_AUTO);
    static const int f[3] = { 10000000, 20000000, 40000000 };
    spi_device_handle_t d = 0;
    for (int k = 0; k < 3; k++) {
        if (d) spi_bus_remove_device(d);
        spi_device_interface_config_t dc = { .clock_speed_hz = f[k], .mode = 0, .spics_io_num = 4, .queue_size = 1 };
        spi_bus_add_device(SPI2_HOST, &dc, &d);
        /* loopback as in IDF's own SPI tests: MISO input fed from the MOSI pad */
        PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[10]);
        esp_rom_gpio_connect_in_signal(10, FSPIQ_IN_IDX, false);
        int act; spi_device_get_actual_freq(d, &act);
        memset(rx, 0, LEN);
        spi_transaction_t t = { .length = 64 * 8, .tx_buffer = tx, .rx_buffer = rx };
        uint32_t c = ccount(); spi_device_polling_transmit(d, &t); uint32_t c64 = ccount() - c;
        int b64 = 0; for (int i = 0; i < 64; i++) b64 += rx[i] != tx[i];
        t.length = LEN * 8;
        memset(rx, 0, LEN);
        c = ccount(); spi_device_transmit(d, &t); uint32_t us = (ccount() - c) / 240;
        int b4k = 0, first = -1; for (int i = 0; i < LEN; i++) if (rx[i] != tx[i]) { b4k++; if (first < 0) first = i; }
        printf("    %2d MHz: 64 B %d wrong (%lu cycles), 4 KB DMA %d wrong, %lu us (wire %lu us)\n",
               act / 1000, b64, c64, b4k, us, (uint32_t)((uint64_t)LEN * 8 * 1000 / act));
    }
    spi_transaction_t t4 = { .length = 32, .tx_buffer = tx, .rx_buffer = rx };
    METRIC("spi_xfer() 4 bytes", "%lu cycles", CYCLES(spi_device_polling_transmit(d, &t4)));
}
