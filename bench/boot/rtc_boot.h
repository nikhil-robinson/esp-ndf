/* Boot-time measurement shared by examples/boot_time and bench/idf_boot (same code on both sides).
 * The RTC timer keeps counting through a chip reset. Boot 1: calibrate RTC ticks against the 16 MHz system timer
 * (20 ms), store the RTC time, software system reset (RTC_CNTL_OPTIONS0 bit 31). Boot 2 (at main / app_main): RTC time
 * again; reset -> main in us into RTC slow memory 0x50001000: [0] magic, [3] calibration, [4] boot us. */
#include <stdint.h>
#define RB_R      ((volatile uint32_t *)0x50001000u)
#define RB_REG(a) (*(volatile uint32_t *)(a))
#define RB_ARMED  0x41524D44u                             /* "ARMD" */
#define RB_DONE   0x444F4E45u                             /* "DONE" */
static inline uint64_t rb_rtc(void)
{
    RB_REG(0x6000800Cu) = 1u << 31;                       /* RTC_CNTL_TIME_UPDATE */
    for (volatile int k = 0; k < 200; k++) {}
    return (uint64_t)RB_REG(0x60008014u) << 32 | RB_REG(0x60008010u);
}
static inline uint64_t rb_sys16(void)                     /* system timer unit 0, 16 MHz */
{
    RB_REG(0x60023004u) = 1u << 30;
    while (!(RB_REG(0x60023004u) & (1u << 29))) {}
    return (uint64_t)RB_REG(0x60023040u) << 32 | RB_REG(0x60023044u);
}
static inline void rb_step(void)                          /* call first thing in main / app_main */
{
    uint64_t now = rb_rtc();
    if (RB_R[0] == RB_ARMED) {
        uint64_t then = (uint64_t)RB_R[2] << 32 | RB_R[1];
        RB_R[4] = (uint32_t)((now - then) * RB_R[3] / 16000u);   /* RB_R[3] = 16 MHz ticks per RTC tick x 1000 */
        RB_R[0] = RB_DONE;
        return;
    }
    if (RB_R[0] == RB_DONE) return;
    uint64_t r0 = rb_rtc(), s0 = rb_sys16();
    while (rb_sys16() - s0 < 320000u) {}                  /* 20 ms */
    uint64_t r1 = rb_rtc(), s1 = rb_sys16();
    RB_R[3] = (uint32_t)((s1 - s0) * 1000u / (r1 - r0));
    uint64_t t = rb_rtc(); RB_R[1] = (uint32_t)t; RB_R[2] = (uint32_t)(t >> 32); RB_R[0] = RB_ARMED;
    RB_REG(0x60008000u) |= 1u << 31;                      /* RTC_CNTL_SW_SYS_RST */
    for (;;) {}
}
