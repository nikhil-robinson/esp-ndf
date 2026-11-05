/* Memory protection lock. */
#include "unit.h"
#include "irq.h"

#include "irq.h"
static void naked_dummy(void) {}
TEST(memprot_locked_refuses_vector_patch)               /* NDF_MEMPROT 2: code stays read-only, the patch is refused */
{
#if SOC_CPU_RISCV    /* P4: handlers go into the CLIC vector table (its own RWX window), no code is patched: nothing to refuse */
    int n = irq_naked(-1, 5, naked_dummy);
    UASSERT(n >= 0);
    irq_detach(-1, n);
#else
    UASSERT_EQ(irq_naked(-1, 5, naked_dummy), -2);
#endif
}

