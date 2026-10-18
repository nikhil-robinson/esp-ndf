/* Keep an output (GPIO4, e.g. a motor driver enable) high through a full
 * system reset with an RTC pad hold; the rebooted firmware takes it over
 * and drives it low 300 ms later. Watch GPIO4 on a scope: no glitch. */
#include "rtcio.h"
#include "sys.h"
#include "console.h"

#define PIN      4
#define STORE3   0x6000805C        /* RTC_CNTL_STORE3, survives system reset */
#define MAGIC    0x484F4C44        /* "HOLD" */

int main(void)
{
    if (REG(STORE3) != MAGIC) {
        rtcio_write(PIN, 1);                           /* level first: no glitch when the output turns on */
        rtcio_config(PIN, GPIO_OUTPUT);
        rtcio_hold(PIN, true);
        REG(STORE3) = MAGIC;
        printf("rtc_hold: GPIO%d high + held, system reset in 200 ms\n", PIN);
        delay_us(200000);
        REG_SET(0x60008000, BIT(31));                  /* RTC_CNTL_OPTIONS0.SW_SYS_RST */
        for (;;) ;
    }
    REG(STORE3) = 0;
    printf("rtc_hold: rebooted, GPIO%d still held: %d; releasing in 300 ms\n", PIN, rtcio_held(PIN));
    delay_us(300000);
    rtcio_write(PIN, 0);                              /* register first, then let go of the hold */
    rtcio_config(PIN, GPIO_OUTPUT);
    rtcio_hold(PIN, false);
    printf("rtc_hold: released, GPIO%d low\n", PIN);
    for (;;) ;
}
