/* Generic main() of a unit-test application (UNIT_MAIN = 1 in its Makefile): runs every registered test
 * (unit.h TEST / test.h NDF_UNIT) and prints "=== unit: P/N passed ===" for tools/ndf_test.py. */
#include "test.h"
#include "sys.h"

int test_fail;

int main(void)
{
    delay_ms(300);                                   /* esptool's sync bytes are still in the USB-JTAG FIFO at boot */
    printf("\n=== %s ===\n", "esp-ndf unit tests");
    int fails = unit_run(NULL);
    printf("=== %s (%d failures) ===\nunit tests done\n", fails ? "FAILED" : "ALL PASS", fails);
    return 0;
}
