/* Exact CALL0 stack unwinder for panic reports. Uses the table that
 * tools/mkunwind.py appends to the image in flash (DWARF CFI reduced to CFA,
 * return-address and a15 rules per address range), read through
 * flash_read: no RAM cost. Interrupt frames are crossed by recognising a
 * return into a level-n entry stub: the interrupted PC is EPCn and the
 * register frame sits at sp (layout of vectors.S SAVE). */
#include "unwind.h"
#include "flash.h"
#include "ndf.h"

#define FRAME   80                       /* vectors.S: saved-register frame */
#define IN_REG  0x7FFF
extern char _text_end[];
#define TEXT_LO 0x40378000u

typedef struct { uint32_t start, end, rows, nrows; } fde_t;
typedef struct { uint16_t pc_off; uint8_t cfa_reg, pad; uint16_t cfa_off; int16_t ra_off, a15_off; } __attribute__((packed)) row_t;

static uint32_t tbl;                     /* flash address of the table once found */
static uint16_t nfde, nirq;

static bool find_table(void)
{
    if (tbl) return true;
    if (nfde == 0xFFFF) return false;    /* looked before: none */
    uint8_t h[24];
    if (flash_read(0, h, sizeof h) || h[0] != 0xE9) { nfde = 0xFFFF; return false; }
    uint32_t off = 24;
    for (int i = 0; i < h[1]; i++) {
        uint32_t seg[2];
        if (flash_read(off, seg, 8)) { nfde = 0xFFFF; return false; }
        off += 8 + seg[1];
    }
    off = (off + 16) & ~15u;             /* checksum ends a 16-byte block */
    if (h[23]) off += 32;                /* SHA-256 */
    off = (off + 255) & ~255u;
    uint8_t m[8];
    if (flash_read(off, m, 8) || m[0] != 'N' || m[1] != 'D' || m[2] != 'F' || m[3] != 'U') { nfde = 0xFFFF; return false; }
    nfde = (uint16_t)(m[4] | m[5] << 8); nirq = (uint16_t)(m[6] | m[7] << 8);
    tbl = off;
    return true;
}

static uint32_t rd32(uint32_t a) { return *(const volatile uint32_t *)a; }
static bool in_ram(uint32_t a, uint32_t sp) { return a >= sp && a < 0x3FCF0000u && !(a & 3); }

static uint32_t epc(int level)
{
    uint32_t v = 0;
    switch (level) {
    case 1: __asm__ volatile("rsr.epc1 %0" : "=r"(v)); break;
    case 2: __asm__ volatile("rsr.epc2 %0" : "=r"(v)); break;
    case 3: __asm__ volatile("rsr.epc3 %0" : "=r"(v)); break;
    case 4: __asm__ volatile("rsr.epc4 %0" : "=r"(v)); break;
    case 5: __asm__ volatile("rsr.epc5 %0" : "=r"(v)); break;
    }
    return v;
}

bool ndf_unwind(uint32_t pc, uint32_t sp, uint32_t a0, uint32_t a15, void (*emit)(uint32_t pc))
{
    if (!find_table()) return false;
    uint32_t base = tbl + 8 + 12u * nirq;
    bool a0_ok = true;
    for (int depth = 0; depth < 32; depth++) {
        emit(pc);
        /* returned into an interrupt entry stub: continue in the interrupted code */
        bool irq = false;
        for (int i = 0; i < nirq; i++) {
            uint32_t e[3];
            flash_read(tbl + 8 + 12u * i, e, 12);
            if (pc >= e[0] && pc < e[1]) {
                if (!in_ram(sp, sp)) return true;
                a0 = rd32(sp);           /* frame word 0: the interrupted a0 */
                pc = epc((int)e[2]);
                sp += FRAME;
                a0_ok = true; irq = true;
                break;
            }
        }
        if (irq) continue;
        /* binary search the function */
        uint32_t look = pc;              /* frames after the first: pc is the call instruction */
        int lo = 0, hi = nfde - 1, found = -1;
        fde_t f;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            flash_read(base + 16u * mid, &f, 16);
            if (look < f.start) hi = mid - 1;
            else if (look >= f.end) lo = mid + 1;
            else { found = mid; break; }
        }
        uint32_t cfa, ra;
        if (found < 0) {                 /* no CFI (assembly): a leaf whose return address is still in a0 */
            if (!a0_ok) return true;
            cfa = sp; ra = a0;
        } else {
            row_t r, best;
            uint32_t rows = base + 16u * nfde + f.rows;
            flash_read(rows, &best, sizeof best);
            for (uint32_t i = 1; i < f.nrows; i++) {
                flash_read(rows + i * sizeof r, &r, sizeof r);
                if (f.start + r.pc_off > look) break;
                best = r;
            }
            cfa = (best.cfa_reg == 15 ? a15 : sp) + best.cfa_off;
            if (best.ra_off == IN_REG) { if (!a0_ok) return true; ra = a0; }
            else { if (!in_ram(cfa + best.ra_off, sp)) return true; ra = rd32(cfa + best.ra_off); }
            if (best.a15_off != IN_REG && in_ram(cfa + best.a15_off, sp)) a15 = rd32(cfa + best.a15_off);
        }
        if (ra < TEXT_LO + 3 || ra >= (uint32_t)_text_end) return true;   /* bottom of the stack */
        pc = ra - 3;                     /* the call instruction */
        sp = cfa;
        a0_ok = false;
    }
    return true;
}
