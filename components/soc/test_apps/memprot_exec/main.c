/* Memory protection: jump into data (a "ret.n" placed in a DRAM buffer, called at its IRAM alias). Expect a panic
 * (fetch blocked) instead of running it. */
#include "console.h"
#include "sys.h"

static uint32_t buf[4] __attribute__((aligned(16))) = { 0x0000f00d, 0, 0, 0 };   /* ret.n */

int main(void)
{
    delay_ms(200);
    void (*fn)(void) = (void (*)(void))((uint32_t)buf + 0x6F0000);
    printf("memprot_exec: calling data at %p\n", (void *)fn);
    fn();
    printf("memprot_exec FAIL: data executed\n");
    for (;;) {}
}
