#include "test.h"
#include "i2c.h"
#include "gpio.h"
#include "sys.h"

/* master I2C0 SDA10/SCL12 <-> jumpers <-> slave I2C1 SDA11/SCL13, addr 0x28 */
#define SLV 0x28

static void bus(uint32_t hz)
{
    uint8_t w[100], r[100], s[8] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7 }, got[8];
    uint32_t act = i2c_master_init(0, 10, 12, hz, true);
    gpio_out_signal(6, 89, false);                           /* mirror master SCL to analyzer D2 */
    gpio_config(6, GPIO_OUTPUT); gpio_out_signal(6, 89, false);
    static bool slave_up;
    if (!slave_up) slave_up = i2c_slave_init(1, 11, 13, SLV, true) == 0;
    printf("    -- %lu Hz (actual %lu) --\n", hz, act);

    for (int i = 0; i < 100; i++) w[i] = i * 7 + 1;
    uint32_t t0 = ccount();
    i2c_err_t e = i2c_master_xfer(0, SLV, w, 16, 0, 0, 100000);
    uint32_t us16 = (ccount() - t0) / (sys_cpu_hz() / 1000000);
    delay_us(200);
    int n = i2c_slave_read(1, r, 100);
    printf("    write 16: err %d, slave got %d, %lu us\n", e, n, us16);
    CHECK(e == I2C_OK && n == 16);
    for (int i = 0; i < 16 && i < n; i++) CHECK(r[i] == w[i]);

    e = i2c_master_xfer(0, SLV, w, 100, 0, 0, 100000);       /* > 32: chunked */
    delay_us(500);
    n = i2c_slave_read(1, r, 100);
    int bad = 0;
    for (int i = 0; i < n; i++) bad += r[i] != w[i];
    printf("    write 100: err %d, slave got %d, %d wrong\n", e, n, bad);
    CHECK(e == I2C_OK && n == 100 && bad == 0);

    i2c_slave_write(1, s, 8);
    e = i2c_master_xfer(0, SLV, 0, 0, got, 8, 100000);
    bad = 0;
    for (int i = 0; i < 8; i++) bad += got[i] != s[i];
    printf("    read 8: err %d, %d wrong (got %02x %02x .. %02x)\n", e, bad, got[0], got[1], got[7]);
    CHECK(e == I2C_OK && bad == 0);

    e = i2c_master_xfer(0, 0x50, w, 1, 0, 0, 100000);       /* nobody at 0x50 */
    printf("    write to 0x50 (absent): err %d (expect %d)\n", e, I2C_NACK);
    CHECK(e == I2C_NACK);
    i2c_slave_read(1, r, 100);
}

/* rise time of an open-drain line with only internal pull-ups: release it
 * from low and time until the far end of the jumper reads high */
static uint32_t rise_ns(int drv, int sense)
{
    gpio_config(drv, GPIO_OUTPUT | GPIO_INPUT | GPIO_OPENDRAIN | GPIO_PULLUP);
    gpio_config(sense, GPIO_INPUT | GPIO_PULLUP);
    uint32_t best = ~0u;
    for (int k = 0; k < 20; k++) {
        gpio_clr(drv); delay_us(20);
        uint32_t t = ccount();
        gpio_set(drv);
        while (!gpio_read(sense)) {}
        uint32_t d = ccount() - t;
        if (d < best) best = d;
    }
    return best * 25 / 6;
}

void test_i2c(void)
{
    uint32_t rs = rise_ns(10, 11), rc = rise_ns(12, 13);
    printf("    bus rise time: SDA %lu ns, SCL %lu ns (spec: 1000 ns @100k, 300 ns @400k)\n", rs, rc);
    irq_enable_all();
    bus(100000);
    printf("    analyzer: SCL 100k burst\n");
    uint8_t w[64] = { 0 };
    for (int k = 0; k < 20; k++) i2c_master_xfer(0, SLV, w, 64, 0, 0, 100000);
    if (rs < 300 && rc < 300) {
        bus(400000);
        printf("    analyzer: SCL 400k burst\n");
        for (int k = 0; k < 40; k++) i2c_master_xfer(0, SLV, w, 64, 0, 0, 100000);
    } else {
        printf("    400 kHz skipped: bus rise time out of Fast-mode spec (add 2.2-4.7k pull-ups)\n");
    }
    gpio_config(6, GPIO_OUTPUT | GPIO_INPUT);
}

NDF_UNIT(010, i2c);
