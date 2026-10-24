/* IDF sibling of examples/periph_test/test_i2c_ext.c: I2C0 master 10/12, I2C1 slave 11/13, 10-bit 0x2A5. */
#include <stdio.h>
#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"

static bool on_recv(i2c_slave_dev_handle_t s, const i2c_slave_rx_done_event_data_t *d, void *arg) { return false; }

void app_main(void)
{
    i2c_slave_config_t sc = { .i2c_port = 1, .sda_io_num = 11, .scl_io_num = 13, .clk_source = I2C_CLK_SRC_DEFAULT,
                              .send_buf_depth = 64, .slave_addr = 0x2A5, .addr_bit_len = I2C_ADDR_BIT_LEN_10 };
    i2c_slave_dev_handle_t sl;
    printf("slave %d\n", i2c_new_slave_device(&sc, &sl));
    i2c_slave_event_callbacks_t cb = { .on_recv_done = on_recv };
    i2c_slave_register_event_callbacks(sl, &cb, NULL);
    static uint8_t rb[8];
    i2c_slave_receive(sl, rb, 8);
    i2c_master_bus_config_t bc = { .i2c_port = 0, .sda_io_num = 10, .scl_io_num = 12, .clk_source = I2C_CLK_SRC_DEFAULT,
                                   .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    i2c_master_bus_handle_t bus;
    i2c_new_master_bus(&bc, &bus);
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_10, .device_address = 0x2A5, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t d;
    i2c_master_bus_add_device(bus, &dc, &d);
    uint8_t w[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    printf("write %d\n", i2c_master_transmit(d, w, 8, 100));
    vTaskDelay(5);
    printf("slave got %d %d %d .. %d\n", rb[0], rb[1], rb[2], rb[7]);
    uint32_t best = ~0u;
    for (int k = 0; k < 5; k++) {
        i2c_slave_receive(sl, rb, 8);
        uint32_t c = esp_cpu_get_cycle_count();
        i2c_master_transmit(d, w, 8, 100);
        c = esp_cpu_get_cycle_count() - c;
        if (c < best) best = c;
        vTaskDelay(2);
    }
    printf("10-bit write 8 B @100k: best %lu cycles\n", best);
    i2c_device_config_t dc2 = dc; dc2.device_address = 0x1A5;
    i2c_master_dev_handle_t d2;
    i2c_master_bus_add_device(bus, &dc2, &d2);
    { uint8_t t4[4] = { 0xC0, 0xC1, 0xC2, 0xC3 }, r4[4] = { 0 }; uint32_t wr;
      i2c_slave_transmit(sl, t4, 4, 100);
      printf("10-bit read: %d %02x %02x\n", i2c_master_receive(d, r4, 4, 100), r4[0], r4[3]); (void)wr; }
    printf("foreign 0x1A5: %d\n", i2c_master_transmit(d2, w, 2, 100));
    for (int a = 0; a < 0x5C; a += 4) printf("  %02x %08lx %08lx\n", a, *(volatile uint32_t *)(0x60013000 + a), *(volatile uint32_t *)(0x60027000 + a));
    rb[0] = 0; i2c_slave_receive(sl, rb, 8);
    printf("then 0x2A5: %d, slave got %d\n", i2c_master_transmit(d, w, 8, 100), (vTaskDelay(3), rb[0]));
    printf("idf_i2c_ext done\n");
}
