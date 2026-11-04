/* ULP-RISC-V: load bench/ulp/ulp_count (90 B), run it every 10 ms for 1 s: the counter it increments must reach ~100,
 * its magic must appear, the mailbox (0x208 -> 0x20C = x + 1) must answer; after ulp_stop the counter must freeze.
 * Load+start cost vs IDF ulp_riscv_load_binary + ulp_riscv_run (bench/idf_ulp). All waits bounded. */
#include "console.h"
#include "ulp.h"
#include "sys.h"
#include "xtensa.h"
#include "rtcclk.h"
#include "ulp/ulp_count.h"
#include "ulp/ulp_fsm_count.h"

int main(void)
{
    delay_ms(200);
    (void)rtc_slow_hz();                                          /* slow-clock calibration (~2 ms, cached), as IDF at boot */
    uint32_t t = ccount();
    int rl = ulp_load(ulp_count_bin, sizeof ulp_count_bin);
    *ulp_mem(0x208) = 41;
    int rr = ulp_run(10000);
    uint32_t tl = ccount() - t;
    uint32_t t0 = ccount(); while (*ulp_mem(0x204) != 0x554C5021u && ccount() - t0 < 240000000u) {}   /* <= 1 s */
    uint32_t first = (ccount() - t0) / 240;
    uint32_t c0 = *ulp_mem(0x200); delay_ms(1000); uint32_t c1 = *ulp_mem(0x200);
    int mbox = *ulp_mem(0x20C) == 42;
    ulp_stop(); delay_ms(50);
    uint32_t c2 = *ulp_mem(0x200); delay_ms(200); uint32_t c3 = *ulp_mem(0x200);
    int ok = !rl && !rr && *ulp_mem(0x204) == 0x554C5021u && c1 - c0 >= 90 && c1 - c0 <= 110 && mbox && c2 == c3;
    printf("%s ULP-RISC-V: load+start %lu us, first run after %lu us, %lu runs in 1 s (10 ms period), mailbox %d, stopped %d\n",
           ok ? "PASS" : "FAIL", (unsigned long)(tl / 240), (unsigned long)first, (unsigned long)(c1 - c0), mbox, c2 == c3);
    /* ULP-FSM */
    int rf = ulp_load(ulp_fsm_count_bin, sizeof ulp_fsm_count_bin) || ulp_fsm_run(0, 10000);
    delay_ms(100);
    uint32_t f0 = *ulp_mem(0x200) & 0xFFFF; delay_ms(1000); uint32_t f1 = *ulp_mem(0x200) & 0xFFFF;
    ulp_stop(); delay_ms(50);
    uint32_t f2 = *ulp_mem(0x200) & 0xFFFF; delay_ms(200); uint32_t f3 = *ulp_mem(0x200) & 0xFFFF;
    int okf = !rf && f1 - f0 >= 90 && f1 - f0 <= 110 && f2 == f3;
    printf("%s ULP-FSM: %lu runs in 1 s (10 ms period), stopped %d\n", okf ? "PASS" : "FAIL", (unsigned long)(f1 - f0), f2 == f3);
    ok &= okf;
    printf("ulp_test %s done\n", ok ? "PASS" : "FAIL");
    for (;;) {}
}
