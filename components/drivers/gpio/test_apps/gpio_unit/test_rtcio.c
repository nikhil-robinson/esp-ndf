#include "test.h"
#include "rtcio.h"
#include "sys.h"

/* Jumpers: GPIO10 <-> 11, 12 <-> 13. Runs first: its second half checks a
 * pad that was held through a full system reset. */
#define STORE3   0x6000805C              /* RTC_CNTL_STORE3: survives system reset */
#define MAGIC    0x52544331              /* "RTC1" */
#define OPTIONS0 0x60008000              /* b31 SW_SYS_RST */

void test_rtcio(void)
{
#if SOC_CPU_RISCV    /* P4: LP IO pads 0..15; the jumpers 10<->11 / 12<->13 are not on this board (docs/P4.md); the S3 test below needs them and RTC_CNTL */
    CHECK(rtcio_config(4, GPIO_OUTPUT | GPIO_INPUT) == 0);
    rtcio_write(4, 1); delay_us(5); CHECK(rtcio_read(4) == 1);          /* a pad reads back what the RTC domain drives */
    rtcio_write(4, 0); delay_us(5); CHECK(rtcio_read(4) == 0);
    rtcio_release(4);
    return;
#endif
    if (REG(STORE3) == MAGIC) {          /* second boot: GPIO10 was held high across the reset */
        REG(STORE3) = 0;
        gpio_config(11, GPIO_INPUT | GPIO_PULLDOWN);
        bool held = rtcio_held(10), level = gpio_read(11);
        rtcio_hold(10, false);
        rtcio_config(10, GPIO_OUTPUT); rtcio_write(10, 0);
        delay_us(5);
        bool after = gpio_read(11);
        printf("    after system reset: pad 10 hold %s, level %d; released -> %d\n", held ? "kept" : "LOST", level, after);
        CHECK(held && level == 1 && after == 0);
        rtcio_release(10);
        gpio_config(10, GPIO_INPUT); gpio_config(11, GPIO_INPUT);
        return;
    }
    /* output through the RTC domain, read back digitally and through RTC IO */
    CHECK(rtcio_config(10, GPIO_OUTPUT) == 0 && rtcio_config(11, GPIO_INPUT) == 0);
    CHECK(rtcio_config(22, GPIO_OUTPUT) == -1 && rtcio_config(19, GPIO_OUTPUT) == -1);
    int ok = 0;
    for (int i = 0; i < 100; i++) { rtcio_write(10, i & 1); delay_us(1); ok += rtcio_read(11) == (i & 1); }
    CHECK(ok == 100);
    METRIC("rtcio_write()", "%lu cycles", CYCLES(rtcio_write(10, 1)));
    METRIC("rtcio_read()", "%lu cycles", CYCLES((void)rtcio_read(11)));
    /* pulls on an RTC input (12 floats except for the jumper to 13) */
    rtcio_config(12, GPIO_INPUT);
    rtcio_config(13, GPIO_INPUT | GPIO_PULLUP);   delay_us(20); bool pu = rtcio_read(12);
    rtcio_config(13, GPIO_INPUT | GPIO_PULLDOWN); delay_us(20); bool pd = rtcio_read(12);
    CHECK(pu == 1 && pd == 0);
    /* open drain: 0 pulls low, 1 lets the pull-up on 11 win */
    rtcio_config(11, GPIO_INPUT | GPIO_PULLUP);
    rtcio_config(10, GPIO_OPENDRAIN);
    rtcio_write(10, 0); delay_us(20); bool od0 = rtcio_read(11);
    rtcio_write(10, 1); delay_us(20); bool od1 = rtcio_read(11);
    CHECK(od0 == 0 && od1 == 1);
    /* hold: the pad keeps its level while the register changes */
    rtcio_config(11, GPIO_INPUT | GPIO_PULLDOWN);
    rtcio_config(10, GPIO_OUTPUT); rtcio_write(10, 1); delay_us(5);
    rtcio_hold(10, true);
    rtcio_write(10, 0); delay_us(5); bool h = rtcio_read(11);
    rtcio_hold(10, false); delay_us(5); bool r = rtcio_read(11);
    CHECK(h == 1 && r == 0);
    printf("    pulls up/down %d/%d, open-drain 0/1 -> %d/%d, held %d, released %d\n", pu, pd, od0, od1, h, r);
    rtcio_release(12); rtcio_release(13); rtcio_release(11);
    /* hold through a system reset: drive 10 high, hold, reset; checked on the next boot */
    rtcio_write(10, 1); delay_us(5);
    rtcio_hold(10, true);
    REG(STORE3) = MAGIC;
    printf("    system reset with GPIO10 held high...\n");
    delay_us(20000);
    REG_SET(OPTIONS0, BIT(31));
    for (;;) ;
}

NDF_UNIT(010, rtcio);
