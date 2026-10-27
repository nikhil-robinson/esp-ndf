/* Two-board harness check: the DEVKIT app only has to boot; tools/wl/selftest.py judges (TEST_HOST). */
#include "console.h"
#include "sys.h"

int main(void)
{
    delay_ms(300);
    printf("wl_selftest: ndf board up\n");
    for (;;) {}
}
