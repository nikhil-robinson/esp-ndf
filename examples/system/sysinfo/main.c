/* System info + configuration check: the banner (NDF_BOOT_BANNER) prints
 * before main(); here we confirm the overrides took effect. */
#include "system.h"
#include "console.h"

extern char _stack0_top[], _stack1_top[];        /* stacks are laid out back to back: core1 above core0 */

int main(void)
{
    sys_info_t i;
    sys_info(&i);
    printf("sysinfo: CPU %lu MHz (config %d), core1 stack %lu B (app.mk 8192)\n",
           i.cpu_hz / 1000000, NDF_CPU_MHZ, (uint32_t)(_stack1_top - _stack0_top));
    printf("sysinfo: partition table at 0x%x, flash read %s\n", NDF_PART_TABLE_ADDR, i.flash_cached ? "cached" : "SPI1");
    for (;;) ;
}
