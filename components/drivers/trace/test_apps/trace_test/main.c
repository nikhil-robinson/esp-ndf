/* App trace over JTAG: waits for OpenOCD ("esp apptrace start"), streams 64 records of 1020 B (u32 seq + pattern),
 * measures CPU cycles per trace_write and the throughput. Host side: tools/trace_test.sh. */
#include "console.h"
#include "sys.h"
#include "trace.h"
#include "cpu.h"

static uint8_t rec[1020] __attribute__((aligned(4)));

int main(void)
{
    trace_init();
    printf("trace_test: waiting for the host\n");
    while (!trace_host_connected()) delay_ms(10);
    uint32_t best = ~0u, fails = 0, t0 = ccount();
    for (uint32_t s = 0; s < 64; s++) {
        for (int i = 0; i < 1020; i++) rec[i] = (uint8_t)(s * 31 + i);
        *(uint32_t *)rec = s;
        uint32_t c = ccount();
        if (trace_write(rec, sizeof rec, 2000000) < 0) fails++;
        c = ccount() - c;
        if (c < best) best = c;
    }
    trace_flush(2000000);
    uint32_t us = (ccount() - t0) / (sys_cpu_hz() / 1000000);
    printf("trace_test: 64 x 1020 B, %lu failed, best write %lu cycles, %lu us = %lu KB/s\n", fails, best, us,
           (uint32_t)(64 * 1024ull * 1000000 / us / 1024));
    for (;;) delay_ms(1000);
}
