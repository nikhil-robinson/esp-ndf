#include "t.h"
#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"
#include "freertos/FreeRTOS.h"
/* master I2C0 SDA10/SCL12 <-> jumpers <-> slave I2C1 SDA11/SCL13, addr 0x28, 100 kHz */
static uint8_t srx[128];
static volatile int s_n;
static bool IRAM_ATTR on_recv(i2c_slave_dev_handle_t s, const i2c_slave_rx_done_event_data_t *e, void *a) { s_n = 1; return false; }
void t_i2c(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = { .i2c_port = 0, .sda_io_num = 10, .scl_io_num = 12, .clk_source = I2C_CLK_SRC_DEFAULT,
                                   .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    i2c_new_master_bus(&bc, &bus);
    i2c_master_dev_handle_t dev;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x28, .scl_speed_hz = 100000 };
    i2c_master_bus_add_device(bus, &dc, &dev);
    i2c_slave_dev_handle_t sl;
    i2c_slave_config_t sc = { .i2c_port = 1, .sda_io_num = 11, .scl_io_num = 13, .clk_source = I2C_CLK_SRC_DEFAULT,
                              .send_buf_depth = 256, .slave_addr = 0x28, .addr_bit_len = I2C_ADDR_BIT_LEN_7 };
    i2c_new_slave_device(&sc, &sl);
    i2c_slave_event_callbacks_t cb = { .on_recv_done = on_recv };
    i2c_slave_register_event_callbacks(sl, &cb, 0);
    uint8_t w[100];
    for (int i = 0; i < 100; i++) w[i] = i * 7 + 1;
    s_n = 0; i2c_slave_receive(sl, srx, 16);
    uint32_t t0 = ccount();
    esp_err_t e = i2c_master_transmit(dev, w, 16, 100);
    uint32_t us = (ccount() - t0) / 240;
    vTaskDelay(2);
    int bad = 0; for (int i = 0; i < 16; i++) bad += srx[i] != w[i];
    printf("    write 16: err %d, slave done %d, %d wrong, %lu us\n", e, s_n, bad, us);
    s_n = 0; i2c_slave_receive(sl, srx, 100);
    e = i2c_master_transmit(dev, w, 100, 100);
    vTaskDelay(5);
    bad = 0; for (int i = 0; i < 100; i++) bad += srx[i] != w[i];
    printf("    write 100: err %d, slave done %d, %d wrong\n", e, s_n, bad);
    i2c_master_dev_handle_t nobody; dc.device_address = 0x50; i2c_master_bus_add_device(bus, &dc, &nobody);
    e = i2c_master_transmit(nobody, w, 1, 100);
    printf("    write to 0x50 (absent): err 0x%x (ESP_ERR_INVALID_STATE/NACK expected)\n", e);
}
