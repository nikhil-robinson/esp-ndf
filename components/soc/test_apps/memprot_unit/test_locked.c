/* Memory protection lock. */
#include "unit.h"
#include "irq.h"

#include "irq.h"
static void naked_dummy(void) {}
TEST(memprot_locked_refuses_vector_patch)               /* NDF_MEMPROT 2: code stays read-only, the patch is refused */
{
    UASSERT_EQ(irq_naked(-1, 5, naked_dummy), -2);
}

