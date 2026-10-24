/* PIE cookbook numbers: cycles per instruction in a zero-overhead loop of 8 copies (throughput) on internal RAM,
 * 16-byte aligned unless stated. Prints a table for docs/PIE.md. */
#include "console.h"
#include "xtensa.h"
#include "sys.h"

static uint8_t buf[4096 + 64] __attribute__((aligned(16)));
#define N 256
#define BENCH(name, setup, body) do { \
    uint32_t t0, t1; \
    __asm__ volatile(setup "\n rsr.ccount %0\n movi a8, 256\n loop a8, 1f\n" body body body body body body body body "\n1:\n rsr.ccount %1" \
                     : "=&r"(t0), "=&r"(t1) : "r"(buf) : "a2", "a3", "a8", "memory"); \
    printf("| `%s` | %lu.%02lu |\n", name, (unsigned long)((t1 - t0) / (8 * N)), (unsigned long)(((t1 - t0) % (8 * N)) * 100 / (8 * N))); \
} while (0)

int main(void)
{
    delay_ms(200);
    printf("| instruction (x8 per loop iteration, 256 iterations) | cycles each |\n|---|---|\n");
    BENCH("nop (loop overhead reference)", "", "nop\n");
    BENCH("ee.vld.128.ip q0 (aligned, +16)", "mov a2, %2", "ee.vld.128.ip q0, a2, 0\n");
    BENCH("ee.vld.128.ip q0 streaming 4 KB", "mov a2, %2", "ee.vld.128.ip q0, a2, 16\n");
    BENCH("ee.vst.128.ip q0", "mov a2, %2", "ee.vst.128.ip q0, a2, 0\n");
    BENCH("ee.ld.128.usar.ip (unaligned load, +1)", "mov a2, %2\n addi a2, a2, 1", "ee.ld.128.usar.ip q0, a2, 0\n");
    BENCH("ee.src.q q2,q0,q1 (unaligned combine)", "", "ee.src.q q2, q0, q1\n");
    BENCH("ee.vmulas.s16.accx q0,q1 (independent)", "", "ee.vmulas.s16.accx q0, q1\n");
    BENCH("ee.vmulas.s8.accx q0,q1", "", "ee.vmulas.s8.accx q0, q1\n");
    BENCH("ee.vmulas.s16.qacc q0,q1 (per lane)", "", "ee.vmulas.s16.qacc q0, q1\n");
    BENCH("ee.vmulas.s8.qacc q0,q1 (per lane)", "", "ee.vmulas.s8.qacc q0, q1\n");
    BENCH("ee.vmulas.s16.accx.ld.ip (MAC + load)", "mov a2, %2", "ee.vmulas.s16.accx.ld.ip q2, a2, 0, q0, q1\n");
    BENCH("ee.vmulas.s8.qacc.ld.ip (MAC + load)", "mov a2, %2", "ee.vmulas.s8.qacc.ld.ip q2, a2, 0, q0, q1\n");
    BENCH("ee.vadds.s16 q2,q0,q1 (independent)", "", "ee.vadds.s16 q2, q0, q1\n");
    BENCH("ee.vadds.s16 q0,q0,q1 (dependent chain)", "", "ee.vadds.s16 q0, q0, q1\n");
    BENCH("ee.vmul.s16 q2,q0,q1", "", "ee.vmul.s16 q2, q0, q1\n");
    BENCH("ee.vmul.s16 q0,q0,q1 (dependent)", "", "ee.vmul.s16 q0, q0, q1\n");
    BENCH("ee.vldbc.16.ip (broadcast load)", "mov a2, %2", "ee.vldbc.16.ip q0, a2, 0\n");
    BENCH("ee.zero.qacc", "", "ee.zero.qacc\n");
    BENCH("ee.srs.accx (read ACCX, shift)", "movi a3, 0", "ee.srs.accx a2, a3, 0\n");
    BENCH("ee.vld.128.ip then dependent vadds (load-use)", "mov a2, %2", "ee.vld.128.ip q0, a2, 0\n ee.vadds.s16 q1, q0, q0\n");
    BENCH("ee.movi.32.a (lane to AR)", "", "ee.movi.32.a q0, a2, 0\n");
    BENCH("FPU madd.s f0 (dependent chain)", "", "madd.s f0, f1, f2\n");
    BENCH("FPU madd.s 4 independent accumulators", "", "madd.s f0, f4, f5\n madd.s f1, f4, f5\n madd.s f2, f4, f5\n madd.s f3, f4, f5\n");
    BENCH("FPU madd.s 8 independent accumulators", "", "madd.s f0, f8, f9\n madd.s f1, f8, f9\n madd.s f2, f8, f9\n madd.s f3, f8, f9\n madd.s f4, f8, f9\n madd.s f5, f8, f9\n madd.s f6, f8, f9\n madd.s f7, f8, f9\n");
    BENCH("FPU add.s dependent chain", "", "add.s f0, f0, f1\n");
    BENCH("FPU add.s independent", "", "add.s f0, f2, f1\n add.s f3, f2, f1\n add.s f4, f2, f1\n add.s f5, f2, f1\n");
    BENCH("FPU mul.s dependent", "", "mul.s f0, f0, f1\n");
    BENCH("FPU lsi then use (load-use)", "mov a2, %2", "lsi f0, a2, 0\n add.s f1, f0, f0\n");
    BENCH("FPU add.s then ssi (result-store)", "mov a2, %2", "add.s f0, f1, f2\n ssi f0, a2, 0\n");
    BENCH("FPU lsi independent", "mov a2, %2", "lsi f0, a2, 0\n");
    BENCH("FPU lsip (load + increment)", "mov a2, %2", "lsip f0, a2, 0\n");
    printf("pie_bench done\n");
    for (;;) {}
}
