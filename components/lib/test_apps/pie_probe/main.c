#include "console.h"
#include <stdint.h>
extern void pie_probe(const void *in, void *out);
extern void pie_probe2(const uint32_t *in, uint32_t *out);
extern void pie_probe3(const uint32_t *in, uint32_t *out);
static uint32_t w[8] __attribute__((aligned(16))) = {0,1,2,3,4,5,6,7}, ov[16] __attribute__((aligned(16)));
static uint8_t a[32] __attribute__((aligned(16))), o[16] __attribute__((aligned(16)));
int main(void)
{
    for (int i = 0; i < 32; i++) a[i] = i;
    uint32_t ms, st;
    __asm__ volatile("csrr %0, mstatus" : "=r"(ms)); __asm__ volatile("csrr %0, 0x7f2" : "=r"(st));
    printf("mstatus %08lx, PIE state %lu\n", (unsigned long)ms, (unsigned long)st);
    __asm__ volatile("csrr %0, mstatus" : "=r"(ms)); __asm__ volatile("csrr %0, 0x7f2" : "=r"(st));
    printf("after: mstatus %08lx, PIE state %lu\n", (unsigned long)ms, (unsigned long)st);
    pie_probe(a, o);
    printf("PIE ran: out %02x %02x %02x %02x\n", o[0], o[1], o[2], o[3]);
    pie_probe2(w, ov);
    printf("unzip.32:"); for (int i = 0; i < 8; i++) printf(" %u", (unsigned)ov[i]); printf("\n");
    printf("zip.32:  "); for (int i = 8; i < 16; i++) printf(" %u", (unsigned)ov[i]); printf("\n");
    pie_probe3(w, ov);
    printf("ld.l/h.64:"); for (int i = 0; i < 4; i++) printf(" %u", (unsigned)ov[i]); printf("\n");
    printf("pie_probe done\n");
    return 0;
}
