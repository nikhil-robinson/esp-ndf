#include "test.h"
#include "i2c.h"
#include "gpio.h"
#include "sys.h"
#include <string.h>

/* I2C0 SDA10/SCL12 <-> jumpers <-> I2C1 SDA11/SCL13 (same bus).
 * 10-bit slave, SCL-held timeout, bus recovery from a slave holding SDA (emulated on core 1),
 * then I2C1 as a 2nd master: both start together, the one sending the larger address loses. */
#define GPIO_IN 0x6000403C
static volatile int go, res1, phase;

static void core1(void)
{
    for (;;) {
        while (!go) {}
        if (phase == 0) {                                   /* stuck slave: SDA low until 3 SCL falls */
            gpio_config(11, GPIO_OUTPUT | GPIO_INPUT | GPIO_OPENDRAIN | GPIO_FORCE);
            gpio_write(11, 0);
            int falls = 0; uint32_t prev = REG(GPIO_IN), t0 = ccount();
            while (falls < 3 && ccount() - t0 < sys_cpu_hz()) {
                uint32_t v = REG(GPIO_IN);
                if ((prev & BIT(13)) && !(v & BIT(13))) falls++;
                prev = v;
            }
            gpio_write(11, 1);
            res1 = falls;
        } else if (phase == 2) {                            /* a device stretching SCL mid-byte for 20 ms */
            uint32_t prev = REG(GPIO_IN), t0 = ccount();
            while (ccount() - t0 < sys_cpu_hz()) { uint32_t v = REG(GPIO_IN); if ((prev & BIT(13)) && !(v & BIT(13))) break; prev = v; }
            gpio_config(13, GPIO_OUTPUT | GPIO_INPUT | GPIO_OPENDRAIN | GPIO_FORCE); gpio_write(13, 0);
            t0 = ccount(); while (ccount() - t0 < (sys_cpu_hz() / 1000) * 20) {}
            gpio_write(13, 1);
        } else {                                            /* second master */
            uint8_t b = 0x55;
            res1 = i2c_master_xfer(1, 0x51, &b, 1, 0, 0, 20000);
        }
        go = 0;
    }
}

void test_i2c_ext(void)
{
    irq_enable_all();
    i2c_master_init(0, 10, 12, 100000, true);
    CHECK(i2c_slave_init(1, 11, 13, I2C_ADDR10 | 0x2A5, true) == 0);
    uint8_t w[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, r[8], s4[4] = { 0xC0, 0xC1, 0xC2, 0xC3 };
    uint32_t tw = ccount();
    i2c_err_t e = i2c_master_xfer(0, I2C_ADDR10 | 0x2A5, w, 8, 0, 0, 100000);
    tw = ccount() - tw;
    delay_us(300);
    int n = i2c_slave_read(1, r, 8);
    i2c_slave_write(1, s4, 4);
    uint8_t rr[4] = { 0 };
    i2c_err_t e2 = i2c_master_xfer(0, I2C_ADDR10 | 0x2A5, 0, 0, rr, 4, 100000);
    i2c_err_t e3 = i2c_master_xfer(0, I2C_ADDR10 | 0x1A5, w, 2, 0, 0, 100000);   /* other 10-bit address */
    printf("    10-bit 0x2A5: write err %d, slave got %d %s; read err %d %02x %02x %02x %02x; 0x1A5 -> %d (NACK)\n",
           e, n, n == 8 && !memcmp(r, w, 8) ? "ok" : "BAD", e2, rr[0], rr[1], rr[2], rr[3], e3);
    METRIC("i2c 10-bit write 8 B @100k", "%lu cycles", tw);
    i2c_err_t e4 = i2c_master_xfer(0, I2C_ADDR10 | 0x2A5, w, 8, 0, 0, 100000);
    delay_us(300);
    int n4 = i2c_slave_read(1, r, 8);
    printf("    after the foreign-address NACK: write to 0x2A5 err %d, slave got %d\n", e4, n4);
    CHECK(e4 == I2C_OK && n4 == 8);
    CHECK(e == I2C_OK && n == 8 && !memcmp(r, w, 8) && e2 == I2C_OK && !memcmp(rr, s4, 4) && e3 == I2C_NACK);

    /* stuck slave holding SDA */
    cpu1_start(core1);
    phase = 0; go = 1;
    delay_us(100);
    bool stuck = !(REG(GPIO_IN) & BIT(10));
    i2c_bus_recover(0);
    uint32_t t0 = ccount(); while (go && ccount() - t0 < (sys_cpu_hz() / 10)) {}
    bool freed = REG(GPIO_IN) & BIT(10);
    printf("    stuck SDA: low %s, recovered after %d clocks, SDA now %s\n", stuck ? "yes" : "NO", res1, freed ? "high" : "LOW");
    CHECK(stuck && res1 == 3 && freed);

    /* a device stretches SCL for 20 ms mid-byte: the hardware timeout (2 ms asked) ends the transfer */
    uint32_t set = i2c_set_timeout(0, 2000);
    phase = 2; go = 1;
    t0 = ccount();
    e = i2c_master_xfer(0, 0x28, w, 4, 0, 0, 100000);
    uint32_t us = (ccount() - t0) / (sys_cpu_hz() / 1000000);
    t0 = ccount(); while (go && ccount() - t0 < (sys_cpu_hz() / 10)) {}
    printf("    SCL stretched 20 ms: err %d (timeout -2) after %lu us, timeout set %lu us\n", e, us, set);
    CHECK(e == I2C_TIMEOUT && us < 15000);
    i2c_set_timeout(0, 200000);
    i2c_master_init(0, 10, 12, 100000, true);

    /* two masters at once: 0x50 (A0) beats 0x51 (A2) at bit 1 */
    i2c_master_init(0, 10, 12, 100000, true);
    i2c_master_init(1, 11, 13, 100000, true);
    uint8_t b = 0xAA;
    int lost0 = 0, lost1 = 0, both_nack = 0, one = 0;
    for (int k = 0; k < 10; k++) {
        phase = 1; go = 1;
        i2c_err_t e0 = i2c_master_xfer(0, 0x50, &b, 1, 0, 0, 20000);
        t0 = ccount(); while (go && ccount() - t0 < (sys_cpu_hz() / 10)) {}
        lost0 += e0 == I2C_ARB_LOST; lost1 += res1 == I2C_ARB_LOST;
        one += (e0 == I2C_ARB_LOST) != (res1 == I2C_ARB_LOST);
        both_nack += e0 == I2C_NACK && res1 == I2C_NACK;    /* not overlapped this round */
        delay_us(200);
    }
    printf("    2 masters x10: I2C0 (0x50) lost %d, I2C1 (0x51) lost %d, exactly one loser %d, no overlap %d\n",
           lost0, lost1, one, both_nack);
    CHECK(one + both_nack == 10 && lost1 > lost0);
    e = i2c_master_xfer(1, 0x51, &b, 1, 0, 0, 20000);
    printf("    loser retries alone: err %d (NACK -1)\n", e);
    CHECK(e == I2C_NACK);
}

NDF_UNIT(020, i2c_ext);
