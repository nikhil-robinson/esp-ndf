#include "unit.h"
#include "console.h"
#include "cpu.h"
#include <string.h>

extern const unit_t __test_start[], __test_end[];
static int failed_now;

void unit_fail(const char *file, int line, const char *msg)
{
    printf("      FAIL %s:%d: %s\n", file, line, msg);
    failed_now = 1;
}

int unit_run(const char *filter)
{
    int n = 0, fails = 0;
    for (const unit_t *t = __test_start; t < __test_end; t++) {
        if (filter && !strstr(t->name, filter)) continue;
        failed_now = 0;
        printf("[%s]\n", t->name);
        uint32_t c = ccount();
        t->fn();
        c = ccount() - c;
        printf("  %s %s (%lu cycles)\n", failed_now ? "FAIL" : "PASS", t->name, (unsigned long)c);
        n++; fails += failed_now;
    }
    printf("=== unit: %d/%d passed ===\n", n - fails, n);
    return fails;
}
