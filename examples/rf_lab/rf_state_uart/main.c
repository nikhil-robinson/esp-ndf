/* Persistent-state hunt: snapshot RTC_CNTL, SENS and every analog register
 * (blocks 0x61..0x6D, regs 0..0x3F) after our init + TX on, then do an
 * RTC_CNTL system reset (OPTIONS0 bit 31) once and snapshot again. The pass
 * number lives in RTC_CNTL_STORE0 (survives a chip reset, maybe not this one). */
#include "console.h"
#include "sys.h"
#include <stdint.h>
#include "rf_regi2c.h"
#include "rf_pll.h"
#include "rf_init.h"

#define R(a) (*(volatile uint32_t *)(a))

int main(void)
{
    delay_ms(1500);
    uint32_t pass = R(0x60008050);
    printf("=== rf_state pass word %08lx\n", (unsigned long)pass);
    printf("@raw\n");                              /* before our init: what survived */
    for (uint32_t a = 0x60008000; a < 0x60008200; a += 4) printf("%08lx %08lx\n", (unsigned long)a, (unsigned long)R(a));
    for (uint32_t a = 0x60008800; a < 0x60008A00; a += 4) printf("%08lx %08lx\n", (unsigned long)a, (unsigned long)R(a));
    for (uint8_t b = 0x61; b <= 0x6D; b++)
        for (uint8_t r = 0; r < 0x40; r++) printf("a%02x%02x %02x\n", b, r, rf_regi2c_read(b, r));
    rf_init_replay();
    rf_pll_result_t pr; rf_pll_set_khz(2437000, &pr);
    rf_rx_on();
    rf_tx_on(127, 0);
    printf("@ours\n");
    for (uint32_t a = 0x60008000; a < 0x60008200; a += 4) printf("%08lx %08lx\n", (unsigned long)a, (unsigned long)R(a));
    for (uint32_t a = 0x60008800; a < 0x60008A00; a += 4) printf("%08lx %08lx\n", (unsigned long)a, (unsigned long)R(a));
    for (uint8_t b = 0x61; b <= 0x6D; b++)
        for (uint8_t r = 0; r < 0x40; r++) printf("a%02x%02x %02x\n", b, r, rf_regi2c_read(b, r));
    rf_tx_off();
    if (pass != 0x5A5A0001u) {
        R(0x60008050) = 0x5A5A0001u;
        printf("=== system reset\n");
        delay_ms(50);
        R(0x60008000) |= 1u << 31;                /* RTC_CNTL_SW_SYS_RST */
    }
    R(0x60008050) = 0;
    printf("=== done\n");
    for (;;) {}
}
