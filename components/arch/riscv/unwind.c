/* Exact stack unwinder for panic reports (RISC-V). Uses the table tools/mkunwind_rv.py appends to the image in flash (per
 * address: CFA = sp + cfa and where ra is saved), read through flash_read_raw (plain SPI1 reads, no driver init): no RAM
 * cost. Interrupt frames are crossed by recognising a return into irq_entry: the interrupted pc, ra and sp come from the
 * frame at sp (vectors.S F_RA / F_MEPC / F_SIZE). */
#include "cache.h"
#include "unwind.h"
#include "flash.h"
#include "ndf.h"

#define IN_REG   0x7FFF
#define NODATA   0x7FFF
#define F_RA     0
#define F_MEPC   64
#define F_SIZE   80

typedef struct { uint32_t pc; int16_t cfa, ra; } row_t;

static uint32_t tbl;
static uint16_t nrows, nirq;

static bool find_table(void)
{
    if (tbl) return true;
    if (nrows == 0xFFFF) return false;
    uint8_t h[24];
    uint32_t base = REG(SOC_OTA_NOTE) == 0x5341544Fu ? REG(SOC_OTA_NOTE + 4) : NDF_BOOT_LOAD_ADDR ? NDF_BOOT_LOAD_ADDR : SOC_FLASH_APP_ADDR;
    if (flash_read_raw(base, h, sizeof h) || h[0] != 0xE9) { nrows = 0xFFFF; return false; }
    uint32_t off = base + 24;
    for (int i = 0; i < h[1]; i++) {
        uint32_t seg[2];
        if (flash_read_raw(off, seg, 8)) { nrows = 0xFFFF; return false; }
        off += 8 + seg[1];
    }
    off = (off + 16) & ~15u;
    if (h[23]) off += 32;
    off = (off + 255) & ~255u;
    uint8_t m[8];
    if (flash_read_raw(off, m, 8) || m[0] != 'N' || m[1] != 'D' || m[2] != 'F' || m[3] != 'V') { nrows = 0xFFFF; return false; }
    nrows = (uint16_t)(m[4] | m[5] << 8); nirq = (uint16_t)(m[6] | m[7] << 8);
    tbl = off;
    return true;
}

static uint32_t rd32(uint32_t a) { return *(const volatile uint32_t *)a; }
static bool in_ram(uint32_t a, uint32_t sp) { return a >= sp && a < 0x4FFC0000u && !(a & 3); }

bool ndf_unwind(uint32_t pc, uint32_t sp, uint32_t a0, uint32_t a15, void (*emit)(uint32_t pc))
{
    (void)a15;
    if (!find_table()) return false;
    uint32_t rbase = tbl + 8 + 8u * nirq;
    bool ra_ok = true;                                 /* ra is valid for the innermost context only */
    for (int depth = 0; depth < 32; depth++) {
        emit(pc);
        bool irq = false;
        for (int i = 0; i < nirq; i++) {
            uint32_t e[2];
            flash_read_raw(tbl + 8 + 8u * i, e, 8);
            if (pc >= e[0] && pc < e[1]) {
                if (!in_ram(sp, sp)) return true;
                a0 = rd32(sp + F_RA); pc = rd32(sp + F_MEPC); sp += F_SIZE;
                ra_ok = true; irq = true;
                break;
            }
        }
        if (irq) continue;
        int lo = 0, hi = (int)nrows - 1, found = -1;   /* last row with pc <= look */
        row_t r;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            flash_read_raw(rbase + 8u * mid, &r, 8);
            if (r.pc <= pc) { found = mid; lo = mid + 1; } else hi = mid - 1;
        }
        uint32_t cfa, ra;
        if (found < 0) return true;
        flash_read_raw(rbase + 8u * found, &r, 8);
        if (r.cfa == NODATA) {                         /* assembly or ROM code: a leaf whose return address is still in ra */
            if (!ra_ok) return true;
            cfa = sp; ra = a0;
        } else {
            cfa = sp + (uint32_t)(int32_t)r.cfa;
            if (r.ra == IN_REG) { if (!ra_ok) return true; ra = a0; }
            else { if (!in_ram(cfa + (uint32_t)(int32_t)r.ra, sp)) return true; ra = rd32(cfa + (uint32_t)(int32_t)r.ra); }
        }
        if (!cache_is_code_addr(ra - 4)) return true;  /* bottom of the stack */
        pc = ra - 4;                                   /* the call instruction (4-byte jal / jalr; 2 for c.jalr is within the same function) */
        sp = cfa;
        ra_ok = false;
    }
    return true;
}
