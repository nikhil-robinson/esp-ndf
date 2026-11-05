/* P4 memory protection (memprot.h) with the RISC-V PMP, as ESP-IDF v6.1's IDRAM split (cpu_region_protect.c,
 * SOC_CPU_IDRAM_SPLIT_USING_PMP, granularity 128 bytes): code (ROM_LO .. _text_end) read + execute, data above it read +
 * write, the .bss / stack / heap window below ROM_LO read + write; nothing else is touched. The M-mode CPU obeys a PMP
 * entry only when it is locked (L), so the entries stay until reset: memprot_init(lock) cannot leave them open, and
 * memprot_code_write() has nothing to open. A violation is a synchronous access fault (mcause 1 / 5 / 7, mtval = the
 * address): the handler fn runs from ndf_fault (sys.c) and the chip panics with the address (the faulting instruction
 * cannot be skipped from the common trap frame). */
#include "memprot.h"
#include "cpu.h"

#define ROM_LO     SOC_IMG_LO
#define DATA_END   (NDF_P4_REV >= 3 ? 0x4FFBCF80u : 0x4FF2BB80u)              /* ROM_HI (link.ld.in) rounded down to the 128-byte granule */
#define LOW_LO     (NDF_P4_REV >= 3 ? 0x4FF20000u : SOC_IMG_LO)
#define PMP_R      1u
#define PMP_W      2u
#define PMP_X      4u
#define PMP_TOR    (1u << 3)
#define PMP_L      0x80u

extern char _text_end[];
static memprot_fn_t viol_fn;
static bool armed;

#define WRITE_PMPADDR(n, v) __asm__ volatile("csrw pmpaddr" #n ", %0" :: "r"(v))

int memprot_init(bool lock)
{
    (void)lock;                                       /* M-mode: only locked entries are enforced */
    if (armed) return 0;
    /* The CLIC fetches a vectored entry from mtvt + 4 * id as an instruction fetch (checked against X): the tables (.mtvt,
     * written by irq_attach) get their own RWX window between the code and the data. */
    extern char _mtvt_end[];
    uint32_t split = ((uint32_t)_text_end + 127u) & ~127u, tv = ((uint32_t)_mtvt_end + 127u) & ~127u;
    WRITE_PMPADDR(0, ROM_LO >> 2);
    WRITE_PMPADDR(1, split >> 2);
    WRITE_PMPADDR(2, tv >> 2);
    WRITE_PMPADDR(3, DATA_END >> 2);
    WRITE_PMPADDR(4, LOW_LO >> 2);
    WRITE_PMPADDR(5, ROM_LO >> 2);
    /* entries: 0 off (base), 1 TOR code RX, 2 TOR vector tables RWX, 3 TOR data RW, 4 off (base), 5 TOR low window RW; entry n is byte n % 4 of pmpcfg(n / 4) */
    uint32_t cfg0 = (uint32_t)(PMP_TOR | PMP_L | PMP_R | PMP_X) << 8 | (uint32_t)(PMP_TOR | PMP_L | PMP_R | PMP_W | PMP_X) << 16 |
                    (uint32_t)(PMP_TOR | PMP_L | PMP_R | PMP_W) << 24;
    uint32_t cfg1 = (uint32_t)(PMP_TOR | PMP_L | PMP_R | PMP_W) << 8;
    __asm__ volatile("csrw pmpcfg0, %0" :: "r"(cfg0));
    __asm__ volatile("csrw pmpcfg1, %0" :: "r"(cfg1));
    __asm__ volatile("fence.i");
    armed = true;
    return 0;
}

void memprot_on_violation(memprot_fn_t fn) { viol_fn = fn; }
bool memprot_code_write(bool open) { (void)open; return !armed; }

/* ndf_fault hook: true when the access fault was a PMP violation we report */
bool memprot_fault(uint32_t cause, uint32_t addr)
{
    if (!armed || !(cause == 1 || cause == 5 || cause == 7)) return false;
    if (viol_fn) viol_fn((int)core_id(), cause == 1 ? MEMPROT_IRAM : MEMPROT_DRAM, addr, cause == 7);
    return true;
}
