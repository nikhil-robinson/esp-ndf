/* memcpy / memset: speed by size and alignment, exhaustive correctness for
 * small sizes and all offsets, and use from inside an interrupt handler. */
#include "console.h"
#include "xtensa.h"
#include "irq.h"
#include "sys.h"
#include <string.h>

static uint8_t A[8192 + 64] __attribute__((aligned(16))), B[8192 + 64] __attribute__((aligned(16)));
static volatile int isr_ok;

static void isr(void *arg)
{
    (void)arg;
    uint8_t tmp[256];
    memcpy(tmp, A + 3, 200); memset(tmp + 200, 0x5A, 56);    /* PIE is off here: must take the scalar path */
    isr_ok = !memcmp(tmp, A + 3, 200) && tmp[255] == 0x5A;
    __asm__ volatile("wsr.ccompare0 %0" :: "r"(ccount() - 1));
}

int main(void)
{
    printf("\n=== memcpy / memset (cycles, MB/s at 240 MHz) ===\n");
    for (int i = 0; i < (int)sizeof A; i++) A[i] = i * 131 + 7;
    static const int sz[] = { 16, 64, 256, 1024, 8192 };
    for (int k = 0; k < 5; k++) {
        int n = sz[k];
        uint32_t t = ccount(); memcpy(B, A, n); uint32_t ta = ccount() - t;
        t = ccount(); memcpy(B + 1, A + 1, n); uint32_t tm = ccount() - t;
        t = ccount(); memcpy(B + 1, A + 2, n); uint32_t tx = ccount() - t;
        t = ccount(); memset(B, 0x33, n); uint32_t ts = ccount() - t;
        printf("  %5d B: memcpy aligned %5lu (%4lu MB/s)  same-offset %5lu  mixed %5lu  memset %5lu (%4lu MB/s)\n",
               n, ta, (uint32_t)n * 240 / ta, tm, tx, ts, (uint32_t)n * 240 / ts);
    }
    /* exhaustive: every size 0..300 x src/dst offsets 0..17 */
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
    printf("  exhaustive sizes 0-300 x offsets: %d wrong\n", bad);
    irq_install(6, isr, 0);                          /* CCOMPARE0, level 1 */
    irq_enable_all();
    __asm__ volatile("wsr.ccompare0 %0; rsync" :: "r"(ccount() + 1000));
    delay_us(100);
    printf("  memcpy/memset inside an interrupt handler: %s\n", isr_ok ? "ok" : "FAILED");
    printf("=== done ===\n");
    for (;;) ;
}
