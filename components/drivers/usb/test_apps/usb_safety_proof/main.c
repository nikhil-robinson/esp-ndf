/* Harmless proof of the OTG recovery path (PHY stays on USB Serial/JTAG): on 'g' from the host, set an RTC_CNTL bit
 * (PAD_HOLD of the unused GPIO21), arm the RTC watchdog with an RTC reset and hang with interrupts off. The RTC reset
 * clears RTC_CNTL (and RTC memory), so the next boot must report the bit as 0 - the OTG PHY select in
 * RTC_CNTL_USB_CONF would be cleared the same way - and this console must be back. Host: tools/usb_safety_proof.py */
#include "console.h"
#include "sys.h"
#include "wdt.h"
#include "xtensa.h"

#define PAD_HOLD 0x600080D8u

int main(void)
{
    printf("usb_safety_proof: boot, PAD_HOLD bit 21 = %lu, send g to arm\n", (REG(PAD_HOLD) >> 21) & 1);
    for (;;) {
        if (console_getc() != 'g') continue;
        REG_SET(PAD_HOLD, BIT(21));
        printf("usb_safety_proof: armed, PAD_HOLD bit 21 = %lu, RTC watchdog (RTC reset) in 2 s, hanging\n", (REG(PAD_HOLD) >> 21) & 1);
        delay_ms(50);
        rtc_wdt_start(2000, true);
        irq_lock();
        for (;;) {}
    }
}
