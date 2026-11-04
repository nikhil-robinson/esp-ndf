/* How long does the timer-group latch take? Cost of each step of gpt_get(). */
#include "gptimer.h"
#include "console.h"
#include "cpu.h"

#define TIMG0     0x6001F000
#define T0_UPDATE (TIMG0 + 0x0C)
#define T0_LO     (TIMG0 + 0x04)
#define T0_HI     (TIMG0 + 0x08)

int main(void)
{
    gpt_init(0, 1000000); gpt_start(0);
    uint32_t polls_max = 0, t_w, t_poll, t_rd;
    for (int i = 0; i < 1000; i++) {
        uint32_t p = 0;
        REG(T0_UPDATE) = BIT(31);
        while (REG(T0_UPDATE) & BIT(31)) p++;
        if (p > polls_max) polls_max = p;
    }
    uint32_t t = ccount(); REG(T0_UPDATE) = BIT(31); t_w = ccount() - t;
    t = ccount(); while (REG(T0_UPDATE) & BIT(31)) {} t_poll = ccount() - t;
    t = ccount(); volatile uint32_t lo = REG(T0_LO), hi = REG(T0_HI); t_rd = ccount() - t; (void)lo; (void)hi;
    uint32_t best = ~0u;
    for (int i = 0; i < 8; i++) { t = ccount(); volatile uint64_t v = gpt_get(0); (void)v; t = ccount() - t; if (t < best) best = t; }
    uint32_t b32 = ~0u;
    for (int i = 0; i < 8; i++) { t = ccount(); volatile uint32_t v = gpt_get32(0); (void)v; t = ccount() - t; if (t < b32) b32 = t; }
    printf("\ngpt_probe: gpt_get32 %lu cycles; ", b32);
    printf("gpt_probe: extra polls max %lu; write %lu, poll %lu, read lo+hi %lu, gpt_get %lu cycles\n", polls_max, t_w, t_poll, t_rd, best);
    for (;;) ;
}
