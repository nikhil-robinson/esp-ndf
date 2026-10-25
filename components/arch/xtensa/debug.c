/* Xtensa data watchpoints (DBREAKA/C, as IDF xt_utils_set_watchpoint) and the debug exception (vectors.S level 6). */
#include "debug.h"
#include "xtensa.h"
#include <stdio.h>

static watch_fn_t watch_fn[SOC_CPU_CORES][2];

int watch_set(int n, const volatile void *addr, size_t len, int mode, watch_fn_t fn)
{
    if (n < 0 || n > 1 || !len || len > 64 || (len & (len - 1)) || ((uint32_t)addr & (len - 1)) || !(mode & 3)) return -1;
    uint32_t c = (0x3Fu << __builtin_ctz(len)) & 0x3F;              /* address bits to compare */
    if (mode & WATCH_READ) c |= BIT(30);
    if (mode & WATCH_WRITE) c |= BIT(31);
    watch_fn[core_id()][n] = fn;
    if (n) __asm__ volatile("wsr %0, dbreaka1\n wsr %1, dbreakc1\n isync" :: "r"(addr), "r"(c));
    else   __asm__ volatile("wsr %0, dbreaka0\n wsr %1, dbreakc0\n isync" :: "r"(addr), "r"(c));
    return 0;
}

void watch_clear(int n)
{
    if (n) __asm__ volatile("wsr %0, dbreakc1\n isync" :: "r"(0));
    else   __asm__ volatile("wsr %0, dbreakc0\n isync" :: "r"(0));
}

void ndf_panic(const char *why, uint32_t pc, const uint32_t *frame, uint32_t sp);

/* from vectors.S, level 6, frame = saved a0-a11... (layout of SAVE) */
RAM_FN void ndf_debug_exc(uint32_t cause, uint32_t pc, uint32_t *frame)
{
    char why[64];
    if (cause & BIT(2)) {                                           /* DB: data break */
        int n = (cause >> 8) & 15;
        watch_fn_t fn = watch_fn[core_id()][n & 1];
        watch_clear(n);
        if (fn) { fn(n, pc); return; }
        snprintf(why, sizeof why, "watchpoint %d hit", n);
    } else snprintf(why, sizeof why, "debug exception, DEBUGCAUSE 0x%02lx", (unsigned long)cause);
    ndf_panic(why, pc, frame, (uint32_t)frame + 80);
}
