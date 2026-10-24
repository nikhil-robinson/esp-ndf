/* RTC slow clock: calibration (timer group 0) against the RTC timer counted over CPU time, switch to 8 MHz/256 and
 * back, 32 kHz crystal start on a board without one (must fail and stay on RC). */
#include "console.h"
#include "rtcclk.h"
#include "sys.h"
#include "xtensa.h"
#include "../../bench/boot/rtc_boot.h"

static uint32_t timer_hz(void)                    /* RTC timer ticks per second, over 50 ms of CPU time */
{
    uint64_t r0 = rb_rtc(); uint32_t c0 = ccount(), lim = sys_cpu_hz() / 20;
    while (ccount() - c0 < lim) {}
    uint64_t r1 = rb_rtc(); uint32_t c1 = ccount();
    return (uint32_t)((r1 - r0) * sys_cpu_hz() / (c1 - c0));
}
static int near(uint32_t a, uint32_t b) { return a > b ? a - b < b / 100 : b - a < b / 100; }   /* within 1 % */

int main(void)
{
    delay_ms(200);
    int fail = 0;
    uint32_t t = ccount(); uint32_t rc = rtc_slow_hz(); uint32_t cal_us = (ccount() - t) / 240;
    uint32_t th = timer_hz();
    printf("%s RC slow clock: calibrated %lu Hz (%lu us), RTC timer %lu Hz\n", near(rc, th) ? "PASS" : "FAIL",
           (unsigned long)rc, (unsigned long)cal_us, (unsigned long)th); fail |= !near(rc, th);
    uint32_t d = slow_clk_cal_hz(SLOW_RC8M_D256, 256);
    int e = slow_clk_select(SLOW_RC8M_D256);
    uint32_t th2 = timer_hz(), hz2 = rtc_slow_hz();
    printf("%s 8 MHz/256: calibrated %lu Hz, selected (%d), RTC timer %lu Hz, rtc_slow_hz %lu\n",
           !e && near(d, th2) && near(hz2, th2) ? "PASS" : "FAIL", (unsigned long)d, e, (unsigned long)th2, (unsigned long)hz2);
    fail |= e || !near(d, th2) || !near(hz2, th2);
    e = slow_clk_select(SLOW_RC);
    uint32_t th3 = timer_hz();
    printf("%s back to RC (%d): RTC timer %lu Hz\n", !e && near(th3, rc) ? "PASS" : "FAIL", e, (unsigned long)th3);
    fail |= e || !near(th3, rc);
    t = ccount(); e = xtal32k_start(false); uint32_t ms = (ccount() - t) / 240000;
    int s = slow_clk_select(SLOW_XTAL32K);
    printf("%s 32 kHz crystal (none fitted): start %d after %lu ms, select %d, still on %d\n",
           e == -1 && s == -1 && slow_clk_src() == SLOW_RC ? "PASS" : "FAIL", e, (unsigned long)ms, s, slow_clk_src());
    fail |= !(e == -1 && s == -1 && slow_clk_src() == SLOW_RC);
    printf("rtcclk_test %s done\n", fail ? "FAIL" : "PASS");
    for (;;) {}
}
