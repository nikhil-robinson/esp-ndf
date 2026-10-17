/* ESP32-S3 CPU interrupt map (TRM table 9.3-2) and interrupt-matrix routing.
 * Included from assembly too: masks are plain hex. */
#pragma once

/* CPU interrupts by priority level (all types) */
#define SOC_INTR_L1_MASK   0x000637FF   /* 0-10, 12, 13, 17, 18          */
#define SOC_INTR_L2_MASK   0x00380000   /* 19-21                          */
#define SOC_INTR_L3_MASK   0x28C08800   /* 11, 15, 22, 23, 27, 29         */
#define SOC_INTR_L4_MASK   0x53000000   /* 24, 25, 28, 30                 */
#define SOC_INTR_L5_MASK   0x84010000   /* 16, 26, 31                     */

/* Level-triggered peripheral interrupts: the ones irq_attach hands out.
 * (Edge 10/22/28/30, NMI 14 and internal 6/7/11/15/16/29 excluded.) */
#define SOC_INTR_PERIPH_LEVEL_MASK 0x8FBE333F

#define SOC_INTR_DISABLED  6    /* internal int: the matrix cannot drive it = disconnected */

#ifndef __ASSEMBLER__
#include "soc.h"

static const uint32_t soc_intr_level_mask[6] = {
    0, SOC_INTR_L1_MASK, SOC_INTR_L2_MASK, SOC_INTR_L3_MASK, SOC_INTR_L4_MASK, SOC_INTR_L5_MASK,
};

static inline int soc_intr_level(int cpu_int)
{
    for (int l = 1; l <= 5; l++) if (soc_intr_level_mask[l] & BIT(cpu_int)) return l;
    return 7;   /* 14 = NMI */
}

static inline void soc_intr_route(int core, int src, int cpu_int)
{
    REG(INTMTX_MAP(core, src)) = cpu_int;
}
#endif
