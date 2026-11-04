/* Brownout probe (built with NDF_BROWNOUT=0: nothing of ours touches the detector): the power-on state of the BOD
 * threshold (analog, regi2c 0x61 reg 5) and of RTC_CNTL_BROWN_OUT, the reset cause, and the comparator output. */
#include "console.h"
#include "regi2c.h"
#include "sys.h"
#include "system.h"
#include "cpu.h"
int main(void)
{
    uint32_t raw = REG(0x60008038u) & 0x3F, bo = REG(0x600080E8u);
    uint8_t th = regi2c_read(0x61, 5);
    delay_ms(300);
    printf("bod_probe: raw cause %lu, BROWN_OUT %08lx (det %lu, ena %lu, rst_ena %lu, rst_sel %lu), BOD reg5 %02x (threshold %u)\n",
           (unsigned long)raw, (unsigned long)bo, (unsigned long)(bo >> 31), (unsigned long)(bo >> 30 & 1),
           (unsigned long)(bo >> 26 & 1), (unsigned long)(bo >> 27 & 1), th, th & 7);
    printf("bod_probe done\n");
    for (;;) {}
}
