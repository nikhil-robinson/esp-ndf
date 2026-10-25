/* App trace, IDF app_trace port/xtensa + app_trace_membufs_proto (target -> host). Two TRAX memory blocks: the CPU
 * fills one while the host reads the other through the TRAX registers over JTAG; a swap re-muxes them
 * (SENSITIVE_INTERNAL_SRAM_USAGE_2). Handshake in TRAX registers through ERI (they are free while TRAX tracing
 * is off): DELAYCNT = control {len 14:0, block id 21:15, host data 22, host connected 23}; TRIGGERPC = "target is
 * swapping" (address the host may break at); PERFMON PM1 = CRC16 of the exposed block | 0xA55A << 16.
 * Every chunk carries {u16 allocated | core << 15, u16 written}. */
#include "trace.h"
#include "xtensa.h"
#include "sys.h"
#include "irq.h"
#include <string.h>

#define ERI_TRAXCTRL   0x100004
#define ERI_TRIGGERPC  0x100014
#define ERI_DELAYCNT   0x10001C
#define ERI_PM1        0x101084
#define SRAM_USAGE_2   0x600C1018
#define BLK0_NUM       28
#define BLK1_NUM       29
#define BLK_SZ         0x4000u
#define CRC_IND        (0xA55Au << 16)
#define HOST_CONNECT   BIT(23)

static uint8_t *const blk[2] = { (uint8_t *)(0x3FC90000u + 0x4000u * (BLK0_NUM - 6)), (uint8_t *)(0x3FC90000u + 0x4000u * (BLK1_NUM - 6)) };
static uint32_t in_block, marker[2], inited;
static volatile uint32_t lk;

static inline uint32_t eri_rd(uint32_t a) { uint32_t v; __asm__ volatile("rer %0, %1" : "=r"(v) : "r"(a)); return v; }
static inline void eri_wr(uint32_t a, uint32_t v) { __asm__ volatile("wer %0, %1; isync" :: "r"(v), "r"(a)); }

static uint32_t mux_bits(int n) { uint32_t a = (n - 2) % 4; return BIT((n - 2) / 4) | a << 14 | BIT(7 + (n - 2) / 4) | a << 16; }
static void select_block(int cpu_blk) { REG(SRAM_USAGE_2) = mux_bits(cpu_blk ? BLK0_NUM : BLK1_NUM); }   /* the other one to TRAX */

static uint16_t crc16_le(const uint8_t *p, uint32_t n)    /* ROM esp_rom_crc16_le(0, ..): reflected 0x1021, ~in/out */
{
    uint16_t c = 0xFFFF;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0x8408 & -(c & 1)); }
    return ~c;
}

void __attribute__((noinline)) trace_eri_updated(void) { __asm__ volatile(".global __esp_apptrace_trax_eri_updated\n__esp_apptrace_trax_eri_updated:"); }

static int swap(void)                                     /* expose the filled block, take the other */
{
    eri_wr(ERI_TRIGGERPC, (uint32_t)trace_eri_updated);   /* lock: the host waits for TRIGGERPC 0 */
    uint32_t ctrl = eri_rd(ERI_DELAYCNT), host = ctrl & HOST_CONNECT;
    if (host && ((ctrl & 0x7FFF) || ((ctrl >> 15) & 0x7F) != (in_block & 0x7F))) {   /* host still reading */
        eri_wr(ERI_TRIGGERPC, 0); trace_eri_updated();
        return -1;
    }
    int prev = in_block % 2, nw = !prev;
    marker[nw] = 0;
    in_block++;
    if (marker[prev]) eri_wr(ERI_PM1, crc16_le(blk[prev], marker[prev]) | CRC_IND);
    select_block(nw);
    eri_wr(ERI_DELAYCNT, (in_block & 0x7F) << 15 | host | (marker[prev] & 0x7FFF));
    eri_wr(ERI_TRIGGERPC, 0); trace_eri_updated();
    return 0;
}

static int swap_wait(uint32_t timeout_us)
{
    uint32_t t0 = ccount(), lim = timeout_us * (sys_cpu_hz() / 1000000);
    while (swap()) { if (ccount() - t0 >= lim) return -1; delay_us(100); }   /* IDF: 100 us between tries on S3 */
    return 0;
}

int trace_init(void)
{
    uint32_t ps = irq_lock();
    if (!inited) { in_block = 0; marker[0] = marker[1] = 0; select_block(0); }
    eri_wr(ERI_TRAXCTRL, BIT(1));                         /* TRSTP */
    eri_wr(ERI_TRAXCTRL, BIT(7));                         /* TMEN */
    eri_wr(ERI_DELAYCNT, 0);                              /* block id 0, nothing exposed */
    eri_wr(ERI_TRIGGERPC, 0);
    inited |= 1u << core_id();
    irq_unlock(ps);
    return 0;
}

bool trace_host_connected(void) { return eri_rd(ERI_DELAYCNT) & HOST_CONNECT; }

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
