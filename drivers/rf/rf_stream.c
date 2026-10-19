/* Gapless continuous capture by bank rotation (scheme after esp_sdr ring.c /
 * h0m3us3r eSpDR capture.c, 0BSD). In circular mode the dump engine writes at
 * a ring index (mod 16384) into whichever bank is selected; moving it to the
 * next bank every THRESHOLD pairs splits the stream into units. Sentinel
 * windows around each unit's predicted start and end locate it exactly, and
 * each unit must start where the previous one ended. */
#include "rf_dump.h"

#define REG(a) (*(volatile uint32_t *)(a))
#define CTRL      0x60033D5Cu
#define WR_INDEX  0x60033D60u
#define CONFIG    0x60033D90u
#define BANK_SEL  0x600C101Cu
#define CIRCULAR  0x00024000u
#define PAIRS     16384u
#define MASK      (PAIRS - 1)
#define SENTINEL  0xA5C33C5Au                   /* bits 31:28 = 0xA: never written by the engine */
#define THRESHOLD 12288u                        /* pairs per unit */
#define START_GUARD 3072u
#define END_GUARD   4096u
#define LATE_LIMIT  2000u
#define SETTLE      16u                         /* pairs after a switch */
#define NOT_FOUND   0xFFFFFFFFu

static inline uint32_t *bank_ptr(unsigned b) { return (uint32_t *)RF_DUMP_BANK(b); }
static inline uint32_t windex(void) { return REG(WR_INDEX) & MASK; }

static void fill(unsigned b, uint32_t at, uint32_t n)
{
    at &= MASK;
    while (n) {
        uint32_t k = PAIRS - at < n ? PAIRS - at : n;
        uint32_t *p = bank_ptr(b) + at, *e = p + k;
        n -= k; at = 0;
        for (; p + 4 <= e; p += 4) { p[0] = SENTINEL; p[1] = SENTINEL; p[2] = SENTINEL; p[3] = SENTINEL; }
        while (p < e) *p++ = SENTINEL;
    }
}

/* first written pair in [o, o + guard): sentinels up to the unit start */
static uint32_t find_first(unsigned b, uint32_t o, uint32_t guard)
{
    const volatile uint32_t *p = bank_ptr(b);
    if (p[o & MASK] != SENTINEL || p[(o + guard - 1) & MASK] == SENTINEL) return NOT_FOUND;
    uint32_t lo = 0, hi = guard - 1;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (p[(o + m) & MASK] == SENTINEL) lo = m + 1; else hi = m; }
    return (o + lo) & MASK;
}

/* one past the last written pair in [o, o + guard), from the index seen at the switch */
static uint32_t find_end(unsigned b, uint32_t o, uint32_t wi, uint32_t guard)
{
    const volatile uint32_t *p = bank_ptr(b);
    uint32_t lo = (wi - o) & MASK;
    if (lo >= guard || p[(o + guard - 1) & MASK] != SENTINEL) return NOT_FOUND;
    uint32_t hi = guard - 1;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (p[(o + m) & MASK] == SENTINEL) hi = m; else lo = m + 1; }
    return (o + lo) & MASK;
}

int rf_stream(uint32_t rate, uint32_t units, rf_stream_cb cb, void *ctx, rf_stream_stats_t *st)
{
    uint32_t sprobe[3] = { 0 }, eprobe[3] = { 0 }, owner = REG(BANK_SEL) & ~15u;
    int rc = 0;
    st->units = 0; st->pairs = 0; st->late_max = 0;
    for (unsigned b = 0; b < 3; b++) fill(b, 0, PAIRS);
    REG(CTRL) = 0;
    REG(CONFIG) = 0x000C2040u;
    REG(CTRL) = CIRCULAR | rate;
    REG(BANK_SEL) = owner | 1;
    REG(CTRL) = CIRCULAR | rate | 1u << 31;
    const uint32_t w0 = windex();
    uint32_t expected = (w0 - 256) & MASK;
    unsigned bank = 0;
    for (uint32_t u = 0;; u++) {
        unsigned next = (bank + 1) % 3;
        int last = u + 1 >= units;
        if (!last) {                            /* next unit's start/end windows */
            sprobe[next] = (expected + THRESHOLD) & MASK;
            eprobe[next] = (sprobe[next] + THRESHOLD) & MASK;
            fill(next, sprobe[next], START_GUARD);
            fill(next, eprobe[next], END_GUARD);
        }
        uint32_t wi, written;
        do { wi = windex(); written = (wi - expected) & MASK; } while (written < THRESHOLD);
        if (written - THRESHOLD > st->late_max) st->late_max = written - THRESHOLD;
        if (written - THRESHOLD > LATE_LIMIT) { rc = -1; break; }   /* the callback took too long */
        if (last) REG(CTRL) = CIRCULAR | rate;  /* stop the engine */
        REG(BANK_SEL) = owner | (last ? 0 : 1u << next);
        while (((windex() - wi) & MASK) < SETTLE && !last) {}
        uint32_t first, end;
        if (u == 0) {
            first = find_first(bank, (w0 - 1024) & MASK, 3072);
            end = find_end(bank, wi, wi, 1024);
        } else {
            first = find_first(bank, sprobe[bank], START_GUARD);
            end = find_end(bank, eprobe[bank], wi, END_GUARD);
        }
        st->first = first; st->expected = expected; st->end = end;
        if (first == NOT_FOUND || (u && first != expected)) { rc = -2; break; }   /* gap or lapped ring (cb too slow) */
        if (end == NOT_FOUND) { rc = -3; break; }
        uint32_t n = (end - first) & MASK, head = PAIRS - first;
        if (head > n) head = n;
        st->units++; st->pairs += n;
        if (cb(bank_ptr(bank) + first, head, bank_ptr(bank), n - head, ctx) || last) break;
        expected = end;
        bank = next;
    }
    REG(CTRL) = 0;
    REG(BANK_SEL) = owner;
    return rc;
}
