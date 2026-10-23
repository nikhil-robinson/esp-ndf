/* Does a software system reset follow RTC_CNTL_PROCPU_STAT_VECTOR_SEL (TRM 10.6) and start the CPU at 0x50000000 (RTC
 * slow memory) without the ROM? Run 1: install stub.S there, vector = RTC, system reset. The stub puts the vector back
 * to the ROM first, stores the system timer + a marker, resets again; run 2 (booted by the ROM) reports. */
#include "console.h"
#include "sys.h"
#include "stub_bin.h"

#define RESET_STATE 0x60008038u
#define MARK        ((volatile uint32_t *)0x50001F00u)

int main(void)
{
    delay_ms(300);
    if (MARK[0] == 0x5354424F) {
        printf("rtcboot_test: STUB RAN after reset, %lu us after reset (system timer); vector now %s\n",
               (unsigned long)(MARK[1] / 16), (REG(RESET_STATE) & BIT(13)) ? "ROM" : "RTC");
        MARK[0] = 0;
        for (;;) {}
    }
    printf("rtcboot_test: installing a %u-byte stub at 0x50000000, vector -> RTC, system reset\n", (unsigned)sizeof stub_words);
    delay_ms(50);
    volatile uint32_t *rtc = (volatile uint32_t *)0x50000000u;
    for (unsigned k = 0; k < sizeof stub_words / 4; k++) rtc[k] = stub_words[k];
    MARK[0] = 0;
    REG_CLR(RESET_STATE, BIT(13));                   /* CPU0 reset vector: 0x50000000 */
    REG_SET(0x60008000u, BIT(31));                   /* RTC_CNTL_SW_SYS_RST */
    for (;;) {}
}
