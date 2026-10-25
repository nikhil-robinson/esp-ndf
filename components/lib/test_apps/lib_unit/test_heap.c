/* Heap integrity and printf hardening. */
#include "unit.h"
#include "heap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *heap_why;
static void heap_hook(const char *w) { heap_why = w; }
TEST(heap_integrity)
{
    heap_on_corrupt = heap_hook;
    UASSERT(heap_check());
    uint8_t *a = malloc(40), *b = malloc(40);
    UASSERT(a && b);
    free(a); heap_why = 0; free(a);                     /* double free */
    UASSERT(heap_why && strstr(heap_why, "double"));
    heap_why = 0; free(a + 4);                          /* stray pointer */
    UASSERT(heap_why && strstr(heap_why, "not a heap"));
    uint8_t keep[16]; memcpy(keep, b + 48, 16);          /* overflow b into the next header */
    memset(b + 48, 0xAA, 16);
    UASSERT(!heap_check());
    heap_why = 0; free(b);
    UASSERT(heap_why && strstr(heap_why, "overflow"));
    memcpy(b + 48, keep, 16);
    UASSERT(heap_check());
    free(b);
    UASSERT(heap_check());
    heap_on_corrupt = 0;
}


#include <stdio.h>
TEST(printf_n_disabled)                                 /* %n must not store anything (NDF_PRINTF_N 0) */
{
    char b[16]; int n = 12345;
    snprintf(b, sizeof b, "abc%n", &n);
    UASSERT_EQ(n, 12345);
}
