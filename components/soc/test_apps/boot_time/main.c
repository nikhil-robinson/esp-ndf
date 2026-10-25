/* Boot time, reset -> main, measured with the RTC timer (keeps counting through the reset): bench/boot/rtc_boot.h,
 * the same code as the IDF sibling bench/idf_boot. Result in RTC memory 0x50001000 [4] (us), also printed. */
#include "console.h"
#include "sys.h"
#include "system.h"
#include "boot/rtc_boot.h"

extern char _bss_start[];
int main(void)
{
    rb_step();                                       /* first: boot 1 arms + resets, boot 2 measures */
    uint32_t rom, init; sys_boot_times(&rom, &init);
    RB_R[5] = rom; RB_R[6] = init;                   /* system timer split: reset -> app first instruction, app init */
    delay_ms(300);
    printf("boot_time: reset -> main %lu us (RTC timer); system timer: first instruction %lu us, init %lu us; RAM image %lu bytes, XIP %d\n",
           (unsigned long)RB_R[4], (unsigned long)rom, (unsigned long)init, (unsigned long)((uint32_t)_bss_start - 0x3FC88000u), NDF_XIP);
    for (;;) {}
}
