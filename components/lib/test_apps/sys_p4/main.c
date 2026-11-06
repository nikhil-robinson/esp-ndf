/* Small kernel-service costs on the P4, cycles per operation (100 in a row, best of 8): GPIO write, critical section enter + exit, time read,
 * 64-byte ring buffer item put + get. */
#include "console.h"
#include "cpu.h"
#include "gpio.h"
#include "irq.h"
#include "ringbuf.h"
#include "sys.h"
#include <string.h>
static uint8_t store[1024] __attribute__((aligned(4)));
int main(void)
{
    gpio_config(20, GPIO_OUTPUT);
    ringbuf_t rb; rb_init(&rb, store, sizeof store);
    uint8_t item[64] = { 1 }, out[64]; volatile uint32_t sink = 0; uint32_t best, t;
#define BEST(name, stmt) do { best = ~0u; for (int r_ = 0; r_ < 8; r_++) { t = ccount(); for (int i = 0; i < 100; i++) { stmt; } t = ccount() - t; if (t < best) best = t; } \
        printf("  %-28s %5lu cycles\n", name, (unsigned long)best / 100); } while (0)
    printf("\nsys_p4 (cycles per operation)\n");
    BEST("gpio_write", gpio_write(20, i & 1));
    BEST("irq_lock + irq_unlock", { uint32_t s = irq_lock(); irq_unlock(s); });
    BEST("time_us", sink += (uint32_t)time_us());
    BEST("ringbuf put+get 64 B", { rb_put(&rb, item, sizeof item); rb_get(&rb, out, sizeof out); });
    printf("sys_p4 done\n");
    for (;;) ;
}
