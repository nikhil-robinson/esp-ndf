/* P-256 field / point primitives in cycles (NDF_ECC_BENCH builds the probe into lib/ecc.c). */
#include "console.h"
#include "sys.h"

extern void p256_bench(void);

int main(void)
{
    delay_ms(300);
    printf("ecc_bench\n");
    p256_bench();
    printf("ecc_bench done\n");
    for (;;) {}
}
