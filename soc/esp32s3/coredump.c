/* Core dump to flash: at a panic, after the console report, write reason, registers, the backtrace and up to 8 KB
 * of the faulting stack into the "coredump" partition. tools/coredump.py reads and decodes it (addr2line).
 * Layout (little endian): magic "NDCD", version 1, core, pc, sp, why[64], frame[20], n_bt, bt[8], stack_len,
 * stack bytes. Off with NDF_COREDUMP 0, or when the table has no coredump partition. */
#include "partition.h"
#include "system.h"
#include <string.h>

#ifndef NDF_COREDUMP
#define NDF_COREDUMP 1
#endif
#define STACK_MAX 8192u

typedef struct { uint32_t magic, version, core, pc, sp; char why[64]; uint32_t frame[20], n_bt, bt[8], stack_len; } cd_hdr_t;
static cd_hdr_t h __attribute__((aligned(4)));

void ndf_coredump(const char *why, uint32_t pc, const uint32_t *frame, uint32_t sp, uint32_t stack_top, int core)
{
    if (!NDF_COREDUMP) return;
    const partition_t *p = partition_find(PART_DATA, PART_SUB_COREDUMP, 0);
    if (!p) return;
    memset(&h, 0, sizeof h);
    h.magic = 0x4443444Eu; h.version = 1; h.core = core; h.pc = pc; h.sp = sp;
    for (int i = 0; why[i] && i < 63; i++) h.why[i] = why[i];
    if (frame) memcpy(h.frame, frame, sizeof h.frame);
    sys_crash_t c;
    extern bool sys_peek_crash(sys_crash_t *c);
    if (sys_peek_crash(&c)) { h.n_bt = c.n_bt; memcpy(h.bt, c.bt, sizeof h.bt); }
    uint32_t len = stack_top > sp && stack_top - sp < 0x20000 ? stack_top - sp : 0;
    if (len > STACK_MAX) len = STACK_MAX;
    h.stack_len = len & ~3u;
    if (partition_erase(p, 0, (sizeof h + h.stack_len + 4095) & ~4095u)) return;
    partition_write(p, sizeof h, (const void *)sp, h.stack_len);   /* body first, header last: a torn dump has no magic */
    partition_write(p, 0, &h, sizeof h);
}
