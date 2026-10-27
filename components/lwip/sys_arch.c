#include "lwip/arch.h"
#include "lwip/sys.h"
#include "sys.h"

u32_t sys_now(void) { return time_ms(); }

void lwip_assert_fail(const char *msg, const char *file, int line)
{
    printf("lwIP assert: %s (%s:%d)\n", msg, file, line);
    ndf_panic("lwip assert", 0, 0, 0);
}
