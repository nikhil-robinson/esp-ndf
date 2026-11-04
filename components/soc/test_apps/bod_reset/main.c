/* Brownout hardware reset mode: boot counter and the raw reset cause of every boot in RTC no-init memory (written
 * first thing, before anything else can reset the chip), printed every second. LEVEL from DEFS (default 7). */
#include "console.h"
#include "brownout.h"
#include "sys.h"
#include "system.h"
#include "cpu.h"
#ifndef LEVEL
#define LEVEL 7
#endif
RTC_NOINIT_ATTR uint32_t magic, boots, causes[16];

int main(void)
{
    uint32_t raw = REG(0x60008038u) & 0x3F;
    if (magic != 0x424F4452u) { magic = 0x424F4452u; boots = 0; }
    causes[boots++ & 15] = raw;
    bod_reset_mode(LEVEL);
    for (uint32_t s = 0;; s++) {
        printf("bod_reset level %d: boot %lu, raw cause %lu (latched %d), up %lu s, BROWN_OUT %08lx; last causes:", LEVEL,
               (unsigned long)boots, (unsigned long)raw, sys_reset_cause(0), (unsigned long)s, (unsigned long)REG(0x600080E8u));
        for (uint32_t k = boots > 8 ? boots - 8 : 0; k < boots; k++) printf(" %lu", (unsigned long)causes[k & 15]);
        printf("\n");
        delay_ms(1000);
    }
}
