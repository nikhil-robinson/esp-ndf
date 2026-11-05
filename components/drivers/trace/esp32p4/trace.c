/* P4 app trace (trace.h): ESP-IDF v6.1 app_trace/port/riscv/port_jtag.c + app_trace_membufs_proto (target -> host). There is no
 * TRAX on the RISC-V cores: two RAM blocks and a control block {ctrl, stat, mem_blocks} that OpenOCD finds through a
 * semihosting call (ebreak sequence, ESP_SEMIHOSTING_SYS_APPTRACE_INIT) and reads / writes over the debug bus.
 * ctrl = {len 14:0, block id 21:15, host data 22, host connected 23} (same bits as the S3's TRAX handshake word),
 * stat = "target is swapping" (address the host may break at). The data cache is in front of the blocks: they are written
 * back after each chunk and the control block is refreshed before it is read. Every chunk carries
 * {u16 allocated | core << 15, u16 written}. */
#include "trace.h"
#include "cpu.h"
#include "sys.h"
#include "irq.h"
#include "cache_p4.h"
#include <string.h>

#ifndef NDF_APPTRACE_BLK
#define NDF_APPTRACE_BLK 8192u
#endif
#define BLK_SZ          NDF_APPTRACE_BLK
#define HOST_CONNECT    BIT(23)
#define ASSIST_DBG_MODE(c) (0x3FF06000u + ((c) ? 0xF4u : 0x74u))     /* b1: debug module active */
#define SEMIHOST_APPTRACE_INIT 0x101

typedef struct { uint8_t *start; uint32_t sz; } mem_block_t;
typedef struct { volatile uint32_t ctrl, stat; mem_block_t *mem; uint8_t pad[64 - 12]; } ctrl_t;
static ctrl_t tc[2] __attribute__((aligned(64)));
static mem_block_t mb[2];
static uint8_t blk[2][BLK_SZ] __attribute__((aligned(64)));
static uint32_t in_block, marker[2], inited;
static volatile uint32_t lk;

static uint32_t ctrl_rd(void)
{
    cache_dma_wbinv((void *)&tc[core_id()], 64);                     /* the host wrote it behind the cache */
    return tc[core_id()].ctrl;
}
static void ctrl_wr(uint32_t v) { tc[core_id()].ctrl = v; cache_dma_writeback((void *)&tc[core_id()], 64); }
static void stat_wr(uint32_t v) { tc[core_id()].stat = v; cache_dma_writeback((void *)&tc[core_id()], 64); }

void __attribute__((noinline)) trace_swapped(void) { __asm__ volatile(".global __esp_apptrace_riscv_updated\n__esp_apptrace_riscv_updated:"); }

static int swap(void)                                     /* expose the filled block, take the other */
{
    stat_wr((uint32_t)trace_swapped);                     /* lock: the host waits for stat 0 */
    uint32_t ctrl = ctrl_rd(), host = ctrl & HOST_CONNECT;
    if (host && ((ctrl & 0x7FFF) || ((ctrl >> 15) & 0x7F) != (in_block & 0x7F))) {   /* host still reading */
        stat_wr(0); trace_swapped();
        return -1;
    }
    int prev = in_block % 2, nw = !prev;
    marker[nw] = 0;
    in_block++;
    cache_dma_writeback(blk[prev], marker[prev]);
    ctrl_wr((in_block & 0x7F) << 15 | host | (marker[prev] & 0x7FFF));
    stat_wr(0); trace_swapped();
    return 0;
}

static int swap_wait(uint32_t timeout_us)
{
    uint32_t t0 = ccount(), lim = timeout_us * (sys_cpu_hz() / 1000000);
    while (swap()) { if (ccount() - t0 >= lim) return -1; delay_us(100); }
    return 0;
}

int trace_init(void)
{
    uint32_t ps = irq_lock();
    int c = (int)core_id();
    if (!inited) { in_block = 0; marker[0] = marker[1] = 0; mb[0].start = blk[0]; mb[0].sz = BLK_SZ; mb[1].start = blk[1]; mb[1].sz = BLK_SZ; }
    tc[c].mem = mb;
    ctrl_wr(0);
    stat_wr(0);
    cache_dma_writeback(mb, sizeof mb);
    inited |= 1u << c;
    irq_unlock(ps);
    if (REG(ASSIST_DBG_MODE(c)) & BIT(1)) {                           /* OpenOCD attached: hand it the control block */
        register long a0 __asm__("a0") = SEMIHOST_APPTRACE_INIT;
        register long a1 __asm__("a1") = (long)&tc[c];
        __asm__ volatile(".option push\n.option norvc\nslli zero, zero, 0x1f\nebreak\nsrai zero, zero, 0x7\n.option pop\n" : "+r"(a0) : "r"(a1) : "memory");
    }
    return 0;
}

bool trace_host_connected(void) { return ctrl_rd() & HOST_CONNECT; }

int trace_write(const void *data, size_t len, uint32_t timeout_us)
{
    if (!(inited & (1u << core_id())) || len + 4 > BLK_SZ) return -1;
    uint32_t ps = irq_lock();
    while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) {}           /* both cores share the blocks */
    int r = 0;
    if (marker[in_block % 2] + len + 4 > BLK_SZ) r = swap_wait(timeout_us);
    if (!r) {
        uint8_t *p = blk[in_block % 2] + marker[in_block % 2];
        uint16_t h[2] = { (uint16_t)(len | core_id() << 15), (uint16_t)len };
        memcpy(p, h, 4); memcpy(p + 4, data, len);
        marker[in_block % 2] += len + 4;
        cache_dma_writeback(p, len + 4);
    }
    __atomic_store_n(&lk, 0, __ATOMIC_RELEASE);
    irq_unlock(ps);
    return r ? -1 : (int)len;
}

int trace_flush(uint32_t timeout_us)
{
    uint32_t ps = irq_lock();
    while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) {}
    int r = marker[in_block % 2] ? swap_wait(timeout_us) : 0;
    __atomic_store_n(&lk, 0, __ATOMIC_RELEASE);
    irq_unlock(ps);
    return r;
}
