/* Peripheral self-tests + timing. Each test_x() runs its checks and prints
 * metrics comparable with bench/idf_periph. */
#include "test.h"
#include "sys.h"

int test_fail;

static void run(const char *name, void (*fn)(void))
{
    int before = test_fail;
    printf("[%s]\n", name);
    fn();
    printf("  %s %s\n", test_fail == before ? "PASS" : "FAIL", name);
}

int main(void)
{
    printf("\n=== esp-ndf peripheral tests ===\n");
    run("gpio", test_gpio);
    run("gptimer", test_gptimer);
    run("ledc", test_ledc);
    run("mcpwm", test_mcpwm);
    run("pcnt", test_pcnt);
    printf("=== %s (%d failures) ===\n", test_fail ? "FAILED" : "ALL PASS", test_fail);
    return 0;
}
