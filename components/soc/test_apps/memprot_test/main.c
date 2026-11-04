/* Memory protection: stores into code through the IRAM address and the DRAM alias are blocked and reported
 * (callback: bus, address, write); data stores above the split work; memprot_code_write opens/closes code.
 * components/soc/test_apps/memprot_exec: executing from data panics. */
#include "console.h"
#include "memprot.h"
#include "sys.h"
#include "cpu.h"

static volatile int hits, hit_bus; static volatile uint32_t hit_addr; static volatile bool hit_wr;
static void on_viol(int core, int bus, uint32_t addr, bool wr) { (void)core; hits++; hit_bus = bus; hit_addr = addr; hit_wr = wr; }
static __attribute__((noinline)) int victim(int x) { return x * 3 + 1; }
static volatile uint32_t data_word;

int main(void)
{
    delay_ms(200);
    memprot_on_violation(on_viol);
    int fail = 0;
    volatile uint32_t *code_i = (volatile uint32_t *)(((uint32_t)victim + 16) & ~3u);
    volatile uint32_t *code_d = (volatile uint32_t *)((uint32_t)code_i - 0x6F0000);
    uint32_t before = *code_d;
    /* 1. store via the DRAM alias */
    hits = 0; *code_d = 0xDEADBEEF; __asm__ volatile("memw"); delay_us(10);
    int ok1 = hits == 1 && hit_bus == MEMPROT_DRAM && hit_wr && (hit_addr & ~15u) == ((uint32_t)code_d & ~15u) && *code_d == before;
    printf("%s DRAM-alias store into code: blocked %d, reported %d (bus %d, addr 0x%08lx, write %d)\n", ok1 ? "PASS" : "FAIL",
           *code_d == before, hits, hit_bus, (unsigned long)hit_addr, hit_wr);
    /* 2. store via the IRAM address */
    hits = 0; *code_i = 0xDEADBEEF; __asm__ volatile("memw"); delay_us(10);
    int ok2 = hits == 1 && hit_bus == MEMPROT_IRAM && hit_wr && *code_d == before;
    printf("%s IRAM store into code: blocked %d, reported %d (bus %d, addr 0x%08lx)\n", ok2 ? "PASS" : "FAIL",
           *code_d == before, hits, hit_bus, (unsigned long)hit_addr);
    /* 3. data store fine, code still runs */
    hits = 0; data_word = 42; int ok3 = data_word == 42 && victim(4) == 13 && hits == 0;
    printf("%s data store and code call unaffected\n", ok3 ? "PASS" : "FAIL");
    /* 4. opened for patching: the store lands; closed again: blocked */
    memprot_code_write(true); *code_d = before ^ 0; uint32_t w = *code_d; memprot_code_write(false);
    hits = 0; *code_d = 0x11111111; __asm__ volatile("memw"); delay_us(10);
    int ok4 = w == before && hits == 1 && *code_d == before;
    printf("%s memprot_code_write: open store ok, closed again blocks (%d)\n", ok4 ? "PASS" : "FAIL", hits);
    fail = !(ok1 && ok2 && ok3 && ok4);
    printf("memprot_test %s done\n", fail ? "FAIL" : "PASS");
    for (;;) {}
}
