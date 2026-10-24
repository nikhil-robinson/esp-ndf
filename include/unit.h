/* On-device unit tests: TEST(name) { ... } registers a test; UASSERT* record failures and return from the test.
 * unit_run(filter) runs the tests whose name contains filter (NULL = all), prints one line per test and
 * "=== unit: P/N passed ===" for host scripts (tools/run_app.py ... "=== unit"). */
#pragma once
#include "ndf.h"

typedef struct { const char *name; void (*fn)(void); const char *file; int line; } unit_t;
#define TEST(n) static void n(void); \
    static const unit_t __test_##n __attribute__((used, aligned(4), section(".ndf_reg.test." #n))) = { #n, n, __FILE__, __LINE__ }; \
    static void n(void)

void unit_fail(const char *file, int line, const char *msg);
#define UASSERT(c) do { if (!(c)) { unit_fail(__FILE__, __LINE__, #c); return; } } while (0)
#define UASSERT_EQ(a, b) do { long long _a = (a), _b = (b); if (_a != _b) { \
    extern int printf(const char *, ...); printf("      %s = %lld, %s = %lld\n", #a, _a, #b, _b); \
    unit_fail(__FILE__, __LINE__, #a " == " #b); return; } } while (0)

int unit_run(const char *filter);   /* failures */
