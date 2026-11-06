#include "console.h"
#include <stdint.h>
extern void pie_probe(const void *in, void *out);
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
    printf("pie_probe done\n");
    return 0;
}
