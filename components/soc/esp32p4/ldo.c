/* P4 on-chip LDO channels (PMU external-LDO units, ESP-IDF v6.1 hal ldo_ll.h): channel 1..4 of the board schematic = unit 0..3.
 * Channel 2 powers the PSRAM (1.8 V), channel 3 the MIPI DSI/CSI PHY (2.5 V) on the Espressif boards. */
#include "ldo.h"
#include "soc.h"
#include "cpu.h"
#include "efuse.h"

#define LDO_UNIT_REG(u)  (PMU_BASE + 0x1B8 + 8 * ((const uint8_t[]){0, 3, 1, 4}[u]))   /* unit = channel - 1 */
#define EFUSE_SYS_2     (SOC_EFUSE_BASE + 0x4C)         /* b31:28 ldo_vo2_dref */
#define EFUSE_SYS_3     (SOC_EFUSE_BASE + 0x50)         /* b5:3 ldo_vo2_mul */

void ldo_on(int unit, int mv)
{
    int best = 0x7FFFFFFF;
    uint32_t dref = 0, mul = 0;
    for (uint32_t d = 0; d < 16; d++) {
        int vref20 = d < 9 ? 10 + (int)d : 20 + ((int)d - 9) * 2;
        for (uint32_t m = 0; m < 8; m++) {
            int err = mv * 80000 - vref20 * 1000 * (4000 + (int)m * 1000);   /* no per-chip trim for this unit */
            if (err < 0) err = -err;
            if (err < best) { best = err; dref = d; mul = m; }
        }
    }
    uint32_t blk = ((REG(EFUSE_SYS_2) >> 11) & 3) * 100 + ((REG(EFUSE_SYS_2) >> 8) & 7);
    if (blk >= 1 && unit == 1 && mv == 1800 && (REG(EFUSE_SYS_2) >> 28) && ((REG(EFUSE_SYS_3) >> 3) & 7)) {
        dref = REG(EFUSE_SYS_2) >> 28;               /* factory-trimmed values */
        mul = (REG(EFUSE_SYS_3) >> 3) & 7;
    }
    uint32_t ctrl = LDO_UNIT_REG(unit), ana = ctrl + 4;
    REG_SET(ana, BIT(27));                          /* current limit on */
    REG_FIELD(ana, 28, 4, dref);
    REG_FIELD(ana, 23, 3, mul);
    REG_CLR(ctrl, BIT(14));                         /* tieh: not the 3.3 V rail */
    REG_SET(ctrl, BIT(7)); REG_FIELD(ctrl, 9, 3, 0);   /* software owner, tieh_sel 0 */
    REG_SET(ana, BIT(26));                          /* ripple suppression */
    REG_SET(ctrl, BIT(8));                          /* xpd */
    REG_CLR(ana, BIT(27));
    for (uint32_t t = ccount(); ccount() - t < 200u * NDF_CPU_MHZ; ) {}   /* stable delay: 200 us */
}

