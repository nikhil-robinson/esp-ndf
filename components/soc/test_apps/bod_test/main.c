/* Brownout detector: at the default level (7, 2.44 V) it must stay quiet for 5 s; at level 1 (3.30 V, about this
 * board's supply) it reports a low supply -> interrupt -> restart with cause RESET_BROWNOUT. */
#include "console.h"
#include "brownout.h"
#include "sys.h"
#include "system.h"
#include "regi2c.h"
#include "xtensa.h"

#define STEP  (*(volatile uint32_t *)0x50001F80u)
#define MAGIC (*(volatile uint32_t *)0x50001F84u)
static void hook(void) { STEP = 2; }

int main(void)
{
    delay_ms(200);
    int cause = sys_reset_cause(0);
    if (MAGIC != 0x424F4454u) { MAGIC = 0x424F4454u; STEP = 0; }
    printf("bod_test step %lu, reset cause %d (%s), supply below level 7: %d\n", (unsigned long)STEP, cause, sys_reset_cause_str(cause), bod_low());
    if (STEP == 0) {
        bool quiet = true;
        for (int i = 0; i < 50; i++) { delay_ms(100); if (bod_low()) quiet = false; if (i % 10 == 0) printf("  %d00 ms\n", i); }
        printf("%s level 7 (2.44 V, boot default): no detection in 5 s\n", quiet ? "PASS" : "FAIL");
        STEP = 1;
        bod_start(1, hook);                                     /* 3.30 V */
        delay_ms(2000);
        printf("level 1 (3.30 V): no detection in 2 s (supply above 3.30 V)\n");
        bod_stop(); MAGIC = 0; printf("bod_test done\n");
    } else if (STEP == 2) {
        printf("%s level 1 (3.30 V) detected the supply, interrupt ran, restart cause %d (%s)\n",
               cause == RESET_BROWNOUT ? "PASS" : "FAIL", cause, sys_reset_cause_str(cause));
        MAGIC = 0; printf("bod_test done\n");
    }
    for (;;) {}
}
