/* Watchdogs over real resets: RTC WDT fed (no reset) / starved (system reset, then chip reset), TG1 MWDT starved,
 * super WDT armed and starved. Reset cause and the real timeout (RTC timer, keeps counting through resets) per step. */
#include "console.h"
#include "wdt.h"
#include "rtcclk.h"
#include "sys.h"
#include "system.h"
#include "cpu.h"
#include "boot/rtc_boot.h"

#define STEP  (*(volatile uint32_t *)0x50001F80u)
#define MAGIC (*(volatile uint32_t *)0x50001F84u)
#define T0LO  (*(volatile uint32_t *)0x50001F88u)
#define T0HI  (*(volatile uint32_t *)0x50001F8Cu)
#define HZ    (*(volatile uint32_t *)0x50001F90u)

static void mark(void) { uint64_t t = rb_rtc(); T0LO = (uint32_t)t; T0HI = (uint32_t)(t >> 32); HZ = rtc_slow_hz(); }
static uint32_t ms_since(void) { uint64_t t = rb_rtc() - ((uint64_t)T0HI << 32 | T0LO); return (uint32_t)(t * 1000 / HZ); }
static uint32_t boot_ms;                         /* reset -> main, taken first */
static void check(const char *what, int want, uint32_t lo, uint32_t hi)
{
    int c = sys_reset_cause(0); uint32_t ms = boot_ms;
    printf("%s %s: cause %d (%s), %lu ms after start\n", c == want && ms >= lo && ms <= hi ? "PASS" : "FAIL", what, c,
           sys_reset_cause_str(c), (unsigned long)ms);
}

int main(void)
{
    if (MAGIC == 0x57445454u) boot_ms = ms_since();
    delay_ms(100);
    int cause = sys_reset_cause(0);
#define CHIPC (*(volatile uint32_t *)0x50001F94u)
    /* RTC-domain resets wipe RTC memory and re-enumerate USB; the host's reconnect then resets once more (cause 21):
     * keep the chip-level cause seen on the first boot for the next one */
    if (cause == RESET_RTCWDT_RTC || cause == RESET_SUPER_WDT) { CHIPC = 0x43480000u | cause; MAGIC = 0x57445454u;
        STEP = cause == RESET_RTCWDT_RTC ? 2 : 5; for (;;) {} }        /* wait for the reconnect reset */
    if ((CHIPC & 0xFFFF0000u) == 0x43480000u) { cause = CHIPC & 0xFF; CHIPC = 0; }
    if (MAGIC != 0x57445454u) { MAGIC = 0x57445454u; STEP = 0; }
    printf("wdt_test step %lu (reset cause %d), RTC slow clock %lu Hz\n", (unsigned long)STEP, cause, (unsigned long)rtc_slow_hz());
    switch (STEP) {
    case 0: {
        uint32_t c0 = ccount(); wdt_feed(); uint32_t f0 = ccount() - c0;
        c0 = ccount(); rtc_wdt_feed(); uint32_t f1 = ccount() - c0;
        c0 = ccount(); swd_feed(); uint32_t f2 = ccount() - c0;
        printf("feed cost: wdt %lu, rtc_wdt %lu, swd %lu cycles\n", (unsigned long)f0, (unsigned long)f1, (unsigned long)f2);
        rtc_wdt_start(100, false);
        for (int i = 0; i < 20; i++) { delay_ms(50); rtc_wdt_feed(); }
        printf("PASS RTC WDT 100 ms fed every 50 ms for 1 s: no reset\n");
        STEP = 1; mark(); rtc_wdt_start(200, false); for (;;) {} }
    case 1:
        check("RTC WDT 200 ms, system reset", RESET_RTCWDT_SYS, 180, 240);
        STEP = 2; mark(); rtc_wdt_start(200, true); for (;;) {}
    case 2:
        printf("%s RTC WDT 200 ms, chip reset: cause %d (%s)\n", cause == RESET_RTCWDT_RTC ? "PASS" : "FAIL", cause, sys_reset_cause_str(cause));
        STEP = 3; mark(); wdt1_start(100, false); for (;;) {}
    case 3:
        check("TG1 WDT 100 ms", RESET_TG1WDT_SYS, 95, 130);
        STEP = 4; sys_restart();
    case 4:
        printf("arming super WDT (not fed)\n");
        swd_enable(true); for (;;) {}
    case 5:
        printf("%s super WDT, not fed: cause %d (%s)\n", cause == RESET_SUPER_WDT ? "PASS" : "FAIL", cause, sys_reset_cause_str(cause));
        swd_enable(false);
        MAGIC = 0;
        printf("wdt_test done\n");
        break;
    }
    for (;;) {}
}
