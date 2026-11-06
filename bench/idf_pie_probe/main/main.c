#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern void pie_probe(const void *in, void *out);
static uint8_t a[32] __attribute__((aligned(16))), o[16] __attribute__((aligned(16)));
void app_main(void)
{
    vTaskDelay(100);
    for (int i = 0; i < 32; i++) a[i] = i;
    uint32_t csr, ms;
    __asm__ volatile("csrr %0, 0x7f2" : "=r"(csr));
    __asm__ volatile("csrr %0, mstatus" : "=r"(ms));
    printf("PIE probe: state before %lu, mstatus %08lx\n", (unsigned long)csr, (unsigned long)ms);
    pie_probe(a, o);
    __asm__ volatile("csrr %0, 0x7f2" : "=r"(csr));
    __asm__ volatile("csrr %0, mstatus" : "=r"(ms));
    printf("PIE probe: survived, state after %lu, mstatus %08lx, out %02x %02x %02x %02x\n", (unsigned long)csr, (unsigned long)ms, o[0], o[1], o[2], o[3]);
}
