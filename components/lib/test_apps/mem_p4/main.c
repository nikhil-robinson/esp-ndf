/* memcpy / memset / memmove / memcmp / strlen on the P4: speed by size and alignment, exhaustive correctness for small
 * sizes and all offsets, and use from inside an interrupt handler (must take the scalar path there). */
#include "console.h"
#include "cpu.h"
#include "irq.h"
#include "sys.h"
#include "soc.h"
#include <string.h>
#include <stdlib.h>

static uint8_t A[8192 + 64] __attribute__((aligned(16))), B[8192 + 64] __attribute__((aligned(16)));
static volatile int isr_ok;
#define CLIC_IP(n) (*(volatile uint8_t *)(0x20801000u + 4u * (16 + (n))))

RAM_FN static void isr(void *arg)
{
    (void)arg;
    uint8_t tmp[256];
    memcpy(tmp, A + 3, 200); memset(tmp + 200, 0x5A, 56);
    isr_ok = !memcmp(tmp, A + 3, 200) && tmp[255] == 0x5A;
}

int main(void)
{
    printf("\n=== memcpy / memset (cycles, MB/s at %lu MHz) ===\n", sys_cpu_hz() / 1000000);
    for (int i = 0; i < (int)sizeof A; i++) A[i] = i * 131 + 7;
    static const int sz[] = { 16, 64, 256, 1024, 8192 };
    for (int k = 0; k < 5; k++) {
        int n = sz[k];
        memcpy(B, A, n); memcpy(B + 1, A + 2, n); memset(B, 1, n); memmove(B + 4, B, n);   /* warm */
        uint32_t t = ccount(); memcpy(B, A, n); uint32_t ta = ccount() - t;
        t = ccount(); memcpy(B + 1, A + 1, n); uint32_t tm = ccount() - t;
        t = ccount(); memcpy(B + 1, A + 2, n); uint32_t tx = ccount() - t;
        t = ccount(); memset(B, 0x33, n); uint32_t ts = ccount() - t;
        t = ccount(); memmove(B + 4, B, n); uint32_t tv = ccount() - t;
        t = ccount(); volatile int r = memcmp(A, B, n); (void)r; uint32_t tc = ccount() - t;
        printf("  %5d B: memcpy aligned %5lu (%4lu MB/s)  same-offset %5lu  mixed %5lu  memset %5lu (%4lu MB/s)  memmove %5lu  memcmp %5lu\n",
               n, ta, (uint32_t)n * (sys_cpu_hz() / 1000000) / ta, tm, tx, ts, (uint32_t)n * (sys_cpu_hz() / 1000000) / ts, tv, tc);
    }
    int bad = 0;
    for (int n = 0; n <= 300; n++)
        for (int so = 0; so < 18; so++)
            for (int dof = 0; dof < 18; dof++) {
                memset(B, 0xEE, 400);
                memcpy(B + dof + 8, A + so, n);
                for (int i = 0; i < 400; i++) {
                    uint8_t want = (i >= dof + 8 && i < dof + 8 + n) ? A[so + i - dof - 8] : 0xEE;
                    if (B[i] != want) { bad++; break; }
                }
            }
    for (int n = 0; n <= 300; n++)
        for (int dof = 0; dof < 18; dof++) {
            memset(B, 0x11, 400); memset(B + dof + 8, 0xC3, n);
            for (int i = 0; i < 400; i++) if (B[i] != ((i >= dof + 8 && i < dof + 8 + n) ? 0xC3 : 0x11)) { bad++; break; }
        }
    /* memmove, overlapping both ways: against a byte reference */
    static uint8_t R[512];
    for (int n = 0; n <= 130; n++)
        for (int so = 0; so < 10; so++)
            for (int dof = 0; dof < 10; dof++) {
                memcpy(B, A, 400); memcpy(R, A, 400);
                memmove(B + dof + 4, B + so + 4, n);
                if (dof <= so) for (int i = 0; i < n; i++) R[dof + 4 + i] = R[so + 4 + i];
                else for (int i = n - 1; i >= 0; i--) R[dof + 4 + i] = R[so + 4 + i];
                if (memcmp(B, R, 400)) bad++;
            }
    /* memcmp / strlen */
    for (int n = 0; n <= 70; n++)
        for (int o = 0; o < 5; o++)
            for (int d = -1; d < n; d++) {
                memcpy(B, A, 100); memcpy(B + 100, A, 100);
                if (d >= 0) B[100 + o + d] ^= 0x80;
                int want = 0; for (int i = 0; i < n; i++) if (A[o + i] != B[100 + o + i]) { want = A[o + i] - B[100 + o + i]; break; }
                int got = memcmp(A + o, B + 100 + o, n);
                if ((got > 0) != (want > 0) || (got < 0) != (want < 0)) bad++;
            }
    for (int n = 0; n < 40; n++)
        for (int o = 0; o < 5; o++) { memset(B, 'x', 100); B[o + n] = 0; if (strlen((char *)B + o) != (size_t)n) bad++; }
    printf("  exhaustive copy / set / move / compare / strlen: %d wrong\n", bad);
    for (int k = 0; k < 2; k++) {                      /* malloc + free, best of 16 */
        size_t n = k ? 4096 : 64; uint32_t best = ~0u;
        for (int i = 0; i < 16; i++) { uint32_t t = ccount(); void *p = malloc(n); free(p); t = ccount() - t; if (t < best) best = t; }
        printf("  malloc+free %u B: %lu cycles\n", (unsigned)n, (unsigned long)best);
    }
    irq_install(1, isr, 0);
    volatile uint8_t *c = &CLIC_IP(1);
    c[3] = (uint8_t)CLIC_LVL_BYTE(1);
    irq_enable_all();
    *c = 1;
    for (int i = 0; i < 1000 && !isr_ok; i++) {}
    printf("  memcpy/memset inside an interrupt handler: %s\n", isr_ok ? "ok" : "FAILED");
    if (!bad && isr_ok) printf("mem_p4 PASS\n");
    for (;;) ;
}
