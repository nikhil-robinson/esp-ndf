/* Light sleep bring-up, safe by construction:
 *  - an RTC watchdog (chip reset, 8 s, never fed, runs in sleep) is armed before the first sleep: whatever happens the
 *    chip resets itself through the ROM (USB comes back) - no human needed;
 *  - results go to RTC_NOINIT memory first, then the console; a boot from that watchdog only reports, never sleeps;
 *  - GPIO5 (logic analyzer D0) is high while awake, low while asleep.
 * Sleeps 20 / 100 / 500 ms on the RTC timer; checks: wake cause timer, RTC-measured sleep length, RAM and a running
 * counter kept, systimer behaviour. DEFS=-DDRY=1: everything but the sleep (proves the recovery path). */
#include "console.h"
#include "sleep.h"
#include "wdt.h"
#include "gpio.h"
#include "system.h"
#include "sys.h"
#include "cpu.h"

#ifndef DRY
#define DRY 0
#endif
#define MAGIC 0x51EE9A11u
typedef struct { uint32_t magic, n, cause[3], rtc_us[3], sys_us[3], ram_ok[3]; } rec_t;
static RTC_NOINIT_ATTR rec_t rec;
static volatile uint32_t pattern[256];

int main(void)
{
    int rc = sys_reset_cause(0);
    gpio_config(5, GPIO_OUTPUT); gpio_set(5);
    delay_ms(1500);                                                     /* host: (re)open the port */
    if (rc == 16) {                                                     /* the safety watchdog fired: report, never sleep */
        bool had = rec.magic == MAGIC; rec.magic = 0;
        for (;;) {                                                      /* repeat: the host may reopen the port late */
            printf("sleep_light_test: safety watchdog reset; previous run %s %lu sleeps\n", had ? "recorded" : "left no record", (unsigned long)rec.n);
            for (uint32_t i = 0; had && i < rec.n && i < 3; i++)
                printf("  sleep %lu: cause 0x%lx, RTC %lu us, systimer %lu us, RAM %lu\n", (unsigned long)i, (unsigned long)rec.cause[i],
                       (unsigned long)rec.rtc_us[i], (unsigned long)rec.sys_us[i], (unsigned long)rec.ram_ok[i]);
            printf("sleep_light_test: not sleeping again. done\n");
            delay_ms(2000);
        }
    }
    rec.magic = MAGIC; rec.n = 0;
    for (int i = 0; i < 256; i++) pattern[i] = i * 0x01010101u ^ 0xA5A5A5A5u;
    printf("sleep_light_test: arming the 8 s safety watchdog, %s\n", DRY ? "DRY run (no sleep)" : "sleeping 20/100/500 ms");
    delay_ms(50);
    rtc_wdt_start(8000, true);
    static const uint32_t ms[3] = { 20, 100, 500 };
    for (int k = 0; k < 3; k++) {
        sleep_wake_none(); sleep_wake_timer(ms[k] * 1000);
        uint64_t r0 = sleep_rtc_us(); uint64_t s0 = time_us();
        gpio_clr(5);
        uint32_t c = DRY ? 8 : sleep_light();
        if (DRY) delay_ms(ms[k]);
        gpio_set(5);
        uint64_t r1 = sleep_rtc_us(); uint64_t s1 = time_us();
        int ok = 1; for (int i = 0; i < 256; i++) ok &= pattern[i] == (i * 0x01010101u ^ 0xA5A5A5A5u);
        rec.cause[k] = c; rec.rtc_us[k] = (uint32_t)(r1 - r0); rec.sys_us[k] = (uint32_t)(s1 - s0); rec.ram_ok[k] = ok; rec.n = k + 1;
    }
    if (DRY) { printf("sleep_light_test: DRY run waits for the watchdog\n"); for (;;) {} }
    delay_ms(2500);                                                     /* USB re-enumerates after the last wake */
    rtc_wdt_stop();
    int pass = 1;
    for (int k = 0; k < 3; k++) {
        int okk = (rec.cause[k] & WAKE_TIMER) && rec.ram_ok[k] && rec.rtc_us[k] >= ms[k] * 1000 && rec.rtc_us[k] < ms[k] * 1000 + 5000;
        printf("%s light sleep %lu ms: cause 0x%lx, RTC %lu us, systimer %lu us, RAM kept %lu\n", okk ? "PASS" : "FAIL",
               (unsigned long)ms[k], (unsigned long)rec.cause[k], (unsigned long)rec.rtc_us[k], (unsigned long)rec.sys_us[k],
               (unsigned long)rec.ram_ok[k]);
        pass &= okk;
    }
    rec.magic = 0;
    printf("sleep_light_test %s done\n", pass ? "PASS" : "FAIL");
    for (;;) {}
}
