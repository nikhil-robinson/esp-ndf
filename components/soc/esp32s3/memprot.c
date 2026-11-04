/* PMS split-line protection, IDF esp_mprot_set_prot sequence (memprot_ll.h): one I/D split line for the main,
 * IRAM0 and DRAM0-DMA lines; IRAM0 area 0 RX, area 3 none; DRAM0 area 0 R, areas 1-3 RW; I-cache memory none. */
#include "memprot.h"
#include "irq.h"
#include "cpu.h"
#include <stdio.h>

#define S(o)            (0x600C1000u + (o))
#define SPLIT_LOCK      S(0xC0)
#define SPLIT(n)        S(0xC4 + 4 * (n))       /* 0 main I/D, 1 IRAM0 line 0, 2 IRAM0 line 1, 3/4 DRAM0 DMA lines */
#define I_PMS_LOCK      S(0xD8)
#define I_PMS           S(0xE0)                 /* world 0: area n at 3n (RWX), cache data arrays at 12, 15 */
#define D_PMS_LOCK      S(0xFC)
#define D_PMS           S(0x100)                /* world 0: area n at 2n (RW) */
#define I_MON_LOCK(c)   S(0xE4 + 0x0C * (c))
#define I_MON_CTRL(c)   S(0xE8 + 0x0C * (c))    /* b0 clr b1 en */
#define I_MON_ST(c)     S(0xEC + 0x0C * (c))    /* b0 intr b1 wr b2 load/store b5.. addr >> 2 */
#define D_MON_LOCK(c)   S(0x104 + 0x10 * (c))
#define D_MON_CTRL(c)   S(0x108 + 0x10 * (c))
#define D_MON_ST(c)     S(0x10C + 0x10 * (c))   /* b0 intr b4.. addr >> 4 */
#define D_MON_ST1(c)    S(0x110 + 0x10 * (c))   /* b0 wr */
#define SRC_I(c)        (85 + 4 * (c))
#define SRC_D(c)        (86 + 4 * (c))
#define R 1u
#define W 2u
#define X 4u

extern char _text_end[];
static memprot_fn_t viol_fn;
static uint32_t i_pms_normal, d_pms_normal;
void ndf_panic(const char *why, uint32_t pc, const uint32_t *frame, uint32_t sp);

static uint32_t split_regval(uint32_t a)        /* category per 64 KB level (2..8), 8 address bits within it */
{
    static const uint32_t hi[7] = { 0x4037FFFF, 0x4038FFFF, 0x4039FFFF, 0x403AFFFF, 0x403BFFFF, 0x403CFFFF, 0x403DFFFF };
    uint32_t v = 0;
    for (int x = 0; x < 7; x++) {
        uint32_t cat = 3;                                               /* above */
        if (a <= hi[x]) { cat = 2; v |= cat << (2 * x); for (int y = x + 1; y < 7; y++) v |= 3u << (2 * y); break; }
        cat = 0;                                                        /* below */
        v |= cat << (2 * x);
    }
    return v | ((a >> 8) & 0xFF) << 14;
}

RAM_FN static void viol_isr(void *a)
{
    int c = core_id(), bus = (int)a;
    uint32_t addr; bool wr;
    if (bus == MEMPROT_IRAM) {
        uint32_t st = REG(I_MON_ST(c));
        if (!(st & 1)) return;
        addr = 0x40000000u + (((st >> 5) & 0xFFFFFF) << 2); wr = st & 2;
        REG(I_MON_CTRL(c)) = BIT(0) | BIT(1); REG(I_MON_CTRL(c)) = BIT(1);   /* clear, stay enabled */
    } else {
        uint32_t st = REG(D_MON_ST(c));
        if (!(st & 1)) return;
        addr = 0x3C000000u + (((st >> 4) & 0x3FFFFF) << 4); wr = REG(D_MON_ST1(c)) & 1;
        REG(D_MON_CTRL(c)) = BIT(0) | BIT(1); REG(D_MON_CTRL(c)) = BIT(1);
    }
    if (viol_fn) { viol_fn(c, bus, addr, wr); return; }
    char why[96];
    snprintf(why, sizeof why, "memory protection: %s %s 0x%08lx (blocked)", bus == MEMPROT_IRAM ? "IRAM bus" : "DRAM bus",
             wr ? "write to" : "fetch/read of", (unsigned long)addr);
    uint32_t sp; __asm__ volatile("mov %0, a1" : "=r"(sp));
    ndf_panic(why, 0, 0, sp);
}

static void monitor_on(int c)
{
    REG(I_MON_CTRL(c)) = BIT(0); REG(I_MON_CTRL(c)) = BIT(1);
    REG(D_MON_CTRL(c)) = BIT(0); REG(D_MON_CTRL(c)) = BIT(1);
    irq_attach(SRC_I(c), 3, viol_isr, (void *)MEMPROT_IRAM);
    irq_attach(SRC_D(c), 3, viol_isr, (void *)MEMPROT_DRAM);
}

static bool arm1_pending;
void memprot_core1(void) { if (arm1_pending) monitor_on(1); }      /* from sys_start1: interrupts are per core */

int memprot_init(bool lock)
{
    if (REG(SPLIT_LOCK) & 1) return -1;
    uint32_t line = ((uint32_t)_text_end + 255) & ~255u;
    REG(I_MON_CTRL(0)) = 0; REG(D_MON_CTRL(0)) = 0; REG(I_MON_CTRL(1)) = 0; REG(D_MON_CTRL(1)) = 0;
    uint32_t v = split_regval(line);
    for (int n = 0; n < 5; n++) REG(SPLIT(n)) = v;
    i_pms_normal = (R | X) << 0 | (R | X) << 3 | (R | X) << 6 | 0u << 9 | 0u << 12 | 0u << 15;
    d_pms_normal = R << 0 | (R | W) << 2 | (R | W) << 4 | (R | W) << 6;
    REG(I_PMS) = (REG(I_PMS) & ~0x3FFFFu) | i_pms_normal;
    REG(D_PMS) = (REG(D_PMS) & ~0xFFu) | d_pms_normal;
    monitor_on(0);
    arm1_pending = true;
    if (lock) {
        REG(SPLIT_LOCK) = 1; REG(I_PMS_LOCK) = 1; REG(D_PMS_LOCK) = 1;
        REG(I_MON_LOCK(0)) = 1; REG(D_MON_LOCK(0)) = 1; REG(I_MON_LOCK(1)) = 1; REG(D_MON_LOCK(1)) = 1;
    }
    return 0;
}

void memprot_on_violation(memprot_fn_t fn) { viol_fn = fn; }

bool memprot_code_write(bool open)
{
    if (!i_pms_normal) return true;                                     /* protection not armed */
    if (REG(I_PMS_LOCK) & 1) return false;
    REG(I_PMS) = (REG(I_PMS) & ~0x3FFFFu) | (open ? i_pms_normal | W : i_pms_normal);
    REG(D_PMS) = (REG(D_PMS) & ~0xFFu) | (open ? d_pms_normal | W : d_pms_normal);
    __asm__ volatile("memw");
    return true;
}
