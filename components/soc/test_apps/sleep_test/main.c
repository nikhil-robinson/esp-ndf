/* Deep sleep: timer wake after 500 ms (measured with the RTC timer through the sleep), RTC_DATA kept, wake cause; then
 * EXT1 wake on a pin already at the wake level (immediate). Sibling: bench/idf_sleep (wake-to-main time). */
#include "console.h"
#include "sleep.h"
#include "wdt.h"
#include "sys.h"
#include "system.h"
#include "xtensa.h"
#include "boot/rtc_boot.h"

RTC_DATA_ATTR uint32_t boots;
RTC_DATA_ATTR uint32_t t0lo, t0hi, step;

int main(void)
{
    uint64_t now = rb_rtc();
    int cause = sys_reset_cause(0);
    rtc_wdt_stop();
    delay_ms(100);
    printf("sleep_test: cause %d (%s), boots %lu, step %lu, wake cause %lx\n", cause, sys_reset_cause_str(cause),
           (unsigned long)boots, (unsigned long)step, (unsigned long)sleep_wake_cause());
    if (cause != RESET_DEEPSLEEP) { boots = 0; step = 0; }
    boots++;
    extern uint32_t rtc_slow_hz(void);
    if (step == 0) {
        step = 1; t0lo = (uint32_t)now; t0hi = (uint32_t)(now >> 32);
        printf("deep sleep 500 ms (timer)\n"); delay_ms(20);
        uint64_t t = rb_rtc(); t0lo = (uint32_t)t; t0hi = (uint32_t)(t >> 32);
        rtc_wdt_start(3000, true);                                     /* safety net: runs in sleep too */
        sleep_wake_timer(500000); sleep_deep();
    }
    if (step == 1) {
        uint32_t ms = (uint32_t)((now - ((uint64_t)t0hi << 32 | t0lo)) * 1000 / rtc_slow_hz());
        bool ok = cause == RESET_DEEPSLEEP && (sleep_wake_cause() & WAKE_TIMER) && boots == 2 && ms >= 495 && ms <= 520;
        printf("%s timer wake: %lu ms from sleep to main, RTC_DATA kept (boots %lu)\n", ok ? "PASS" : "FAIL", (unsigned long)ms, (unsigned long)boots);
        step = 2; printf("deep sleep, EXT1 wake on GPIO0 (BOOT button, idle high: any-high -> at once)\n"); delay_ms(20);
        rtc_wdt_start(3000, true);
        sleep_wake_none(); sleep_wake_ext1(BIT(0), true); sleep_deep();
    }
    if (step == 2) {
        bool ok = cause == RESET_DEEPSLEEP && (sleep_wake_cause() & WAKE_EXT1) && (sleep_ext1_pins() & BIT(0));
        printf("%s EXT1 wake: cause %lx, pins %lx\n", ok ? "PASS" : "FAIL", (unsigned long)sleep_wake_cause(), (unsigned long)sleep_ext1_pins());
        step = 0; printf("sleep_test done\n");
    }
    for (;;) {}
}
