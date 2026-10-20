/* Hardware BLE receive without the vendor stack: our PHY init, then the BLE modem block, the
 * link-layer core and its exchange memory from the captured vendor state (drivers/ble/ble_state.h),
 * then our own scan events kicked on ET entry 0. The core demodulates, checks the CRC and writes
 * packets into the RX ring by itself; we only read descriptors. */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include <stdint.h>
#include <string.h>
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"
#include "../../drivers/rf/rf_regi2c.h"
#include "../../drivers/ble/ble_state.h"
#include "../../drivers/ble/ble_idf_fix.h"

#define R32(a)  (*(volatile uint32_t *)(a))
#define CORE(o) R32(0x60031000u + (o))
#define em_buf ((uint32_t *)0x3FCB0000u)            /* SRAM bank the vendor EM sat in; the dump engine is unused here */

static uint32_t em_addr(uint32_t off)
{
    unsigned b = 0;
    for (unsigned k = 0; k < sizeof ble_em_segs / sizeof ble_em_segs[0]; k++)
        if (ble_em_segs[k].em_off <= off && ble_em_segs[k].em_off >= ble_em_segs[b].em_off) b = k;
    return (uint32_t)em_buf + ble_em_segs[b].pos + off - ble_em_segs[b].em_off;
}
#define EM16(off) (*(volatile uint16_t *)em_addr(off))

static uint32_t clkn(void)
{
    CORE(0x1C) = (CORE(0x1C) & 0x7FFFFFFFu) | 0x80000000u;
    while (CORE(0x1C) & 0x80000000u) {}
    return CORE(0x1C) & 0x0FFFFFFFu;
}

static void ble_setup(void)
{
    R32(0x60026014) = 0xFFFFDFDFu;                  /* SYSCON clocks as the vendor leaves them (BT included) */
    R32(0x60026018) = 0;
    /* BT baseband config 0x6000E000, as libbtbb bt_bb_v2_init_cmplx loads it: bit 25 of E0C4 held while
     * the setters write, then released (a load strobe: final values alone do nothing) */
    R32(0x6000E0C4) |= 1u << 25;
    for (unsigned k = 0; k < sizeof ble_vendor_phy / sizeof ble_vendor_phy[0]; k++) {
        uint32_t a = ble_vendor_phy[k].addr;
        if (a >= 0x6000E000u && a < 0x6000E200u && a != 0x6000E0C4u) R32(a) = ble_vendor_phy[k].val;
    }
    R32(0x600060FC) = (R32(0x600060FC) & 0xFFFF00FFu) | 0xC800u;
    R32(0x6000E0C4) = 0xC142287Cu & ~(1u << 25);      /* vendor final value, strobe released */
    for (int k = 0; k < 64; k++) R32(0x60011000u + 4 * k) = ble_modem_regs[k];
    memcpy(em_buf, ble_em_image, sizeof ble_em_image);
    for (unsigned k = 0; k < sizeof ble_em_segs / sizeof ble_em_segs[0]; k++) {
        unsigned i = ble_em_segs[k].idx;
        uint32_t a = (uint32_t)em_buf + ble_em_segs[k].pos;
        R32(0x60031204u + 4 * i + (i > 47 ? 28 : 0)) = ((uint32_t)ble_em_segs[k].em_off >> 2) << 18 | ((a >> 2) & 0x3FFFF);
    }
    for (unsigned k = 0; k < sizeof ble_core_regs / sizeof ble_core_regs[0]; k++) CORE(ble_core_regs[k].off) = ble_core_regs[k].val;
    CORE(0x0C) = 0;                                 /* polled */
    CORE(0x00) = ble_rwblecntl;
}

#define PR printf
static void kick_state_dump(void)
{
    static const struct { uint32_t base, len; } blk[] = {
        { 0x60011000, 0x1000 }, { 0x60031000, 0x800 }, { 0x60006000, 0x200 }, { 0x6001C000, 0x2000 }, { 0x6000E000, 0x200 },
        { 0x60026000, 0x100 }, { 0x60008000, 0x140 }, { 0x600C0000, 0x100 }, { 0x60033000, 0x3000 },
    };
    PR("== kstate\n");
    for (unsigned b = 0; b < sizeof blk / sizeof blk[0]; b++)
        for (uint32_t a = blk[b].base; a < blk[b].base + blk[b].len; a += 4) PR("r %08lx %08lx\n", (unsigned long)a, (unsigned long)*(volatile uint32_t *)a);
    static const uint8_t ab[] = { 0x61, 0x62, 0x63, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D };
    for (unsigned b = 0; b < sizeof ab; b++) for (int r = 0; r < 32; r++) PR("a %02x %02x %02x\n", ab[b], r, rf_regi2c_read(ab[b], r));
    PR("== kstate end\n");
}

static int got;
static int run_events(int n, int verbose)
{
    int hits = 0;
    for (int t = 0; t < n; t++) {
        for (uint32_t d = 0x1000; d < 0x10C8; d += 0x14) { EM16(d) &= 0x7FFF; EM16(d + 4) = 0; }
        uint32_t ck = clkn();
        EM16(0) = EM16(0) & ~0x38u;
        EM16(2) = (ck + 8) & 0xFFFF; EM16(4) = ((ck + 8) >> 16) & 0xFFF; EM16(6) = 0;
        CORE(0x24) = 0x1000;
        CORE(0x18) = 0xFFFFFFFFu;
        CORE(0x100) = 0x80000000u;
        uint32_t t0 = ccount(), hit = 0;
        while (!hit && ccount() - t0 < 240000u * 40)
            for (uint32_t d = 0x1000; d < 0x10C8; d += 0x14) if (EM16(d) & 0x8000) { hit = d; break; }
        if (!hit) continue;
        hits++;
        if (verbose && got++ < 5) {
            uint16_t ph = EM16(hit + 4), dp = EM16(hit + 0x12); int len = ph >> 8;
            printf("  RX hdr %04x ch %d rssi %d:", ph, (EM16(hit + 6) >> 8) & 0x3F, (int8_t)EM16(hit + 6));
            for (int k = 0; k < len && k < 37; k++) printf(" %02x", *(volatile uint8_t *)(em_addr(dp) + k));
            printf("\n");
        }
    }
    return hits;
}

int main(void)
{
    delay_ms(1500);
    printf("=== ble_hwrx (no vendor code)\n");
    rf_init_replay();
    rf_pll_result_t r;
    printf("pll rc %d locked %d\n", rf_pll_set_khz(2402000, &r), r.locked);
    rf_freq_mem_load();
    printf("pll rc %d locked %d (after freq mem)\n", rf_pll_set_khz(2402000, &r), r.locked);
    rf_rx_on();
#ifdef VENDOR_FIRST
    {   /* every captured vendor block before the BLE setup */
        int nch = 0;
        for (unsigned b = 0; b < sizeof ble_vendor_phy_blocks / sizeof ble_vendor_phy_blocks[0]; b++) {
            if (!((VENDOR_FIRST >> b) & 1)) continue;
            uint32_t lo = ble_vendor_phy_blocks[b].base, hi = lo + ble_vendor_phy_blocks[b].len;
            for (unsigned k = 0; k < sizeof ble_vendor_phy / sizeof ble_vendor_phy[0]; k++) {
                uint32_t a = ble_vendor_phy[k].addr;
                if (a >= lo && a < hi && R32(a) != ble_vendor_phy[k].val) { R32(a) = ble_vendor_phy[k].val; nch++; }
            }
        }
        if (VENDOR_FIRST & 0x1000) for (unsigned k = 0; k < sizeof ble_vendor_analog / sizeof ble_vendor_analog[0]; k++) {
            const uint8_t *v = ble_vendor_analog[k];
            if (v[0] != 0x62 && v[0] != 0x63 && rf_regi2c_read(v[0], v[1]) != v[2]) { rf_regi2c_write(v[0], v[1], v[2]); nch++; }
        }
        printf("vendor blocks first: %d writes\n", nch);
    }
#endif
    ble_setup();
    // kick_state_dump();
    printf("core ver %08lx clkn %07lx\n", (unsigned long)CORE(0x04), (unsigned long)clkn());
    uint8_t n0 = rf_regi2c_read(0x63, 3), f0 = rf_regi2c_read(0x63, 4);
    uint32_t b0 = R32(0x600110B0);
#ifdef IDF_FIX
    for (unsigned k = 0; k < sizeof ble_idf_fix / sizeof ble_idf_fix[0]; k++) if ((IDF_FIX >> (k & 31)) & 1) R32(ble_idf_fix[k].addr) = ble_idf_fix[k].val;
    if (IDF_FIX_ANA) for (unsigned k = 0; k < sizeof ble_idf_fix_ana / sizeof ble_idf_fix_ana[0]; k++) rf_regi2c_write(ble_idf_fix_ana[k][0], ble_idf_fix_ana[k][1], ble_idf_fix_ana[k][2]);
#endif
    printf("stage base: %d/10\n", run_events(10, 1));
#ifdef ONLY_BASE
    printf("modem b0 %08lx 70 %08lx\nreceived %d\n=== done\n", (unsigned long)R32(0x600110B0), (unsigned long)R32(0x60011070), got);
    for (;;) {}
#endif
    printf("  modem b0 %08lx -> %08lx, pll N %02x.%02x -> %02x.%02x, 6c %08lx 70 %08lx 7c %08lx 80 %08lx\n", (unsigned long)b0, (unsigned long)R32(0x600110B0),
           n0, f0, rf_regi2c_read(0x63, 3), rf_regi2c_read(0x63, 4), (unsigned long)R32(0x6001106C), (unsigned long)R32(0x60011070),
           (unsigned long)R32(0x6001107C), (unsigned long)R32(0x60011080));
    const unsigned nb = sizeof ble_vendor_phy_blocks / sizeof ble_vendor_phy_blocks[0];
    for (unsigned b = 0; b < nb; b++) {
        int nch = 0;
        uint32_t lo = ble_vendor_phy_blocks[b].base, hi = lo + ble_vendor_phy_blocks[b].len;
        for (unsigned k = 0; k < sizeof ble_vendor_phy / sizeof ble_vendor_phy[0]; k++) {
            uint32_t a = ble_vendor_phy[k].addr;
            if (a >= lo && a < hi && R32(a) != ble_vendor_phy[k].val) { R32(a) = ble_vendor_phy[k].val; nch++; }
        }
        printf("stage +%08lx (%d regs): %d/10\n", (unsigned long)lo, nch, run_events(10, 1));
    }
    int na = 0;
    for (unsigned k = 0; k < sizeof ble_vendor_analog / sizeof ble_vendor_analog[0]; k++) {
        const uint8_t *v = ble_vendor_analog[k];
        if (v[0] == 0x62 || v[0] == 0x63) continue;     /* PLL stays ours */
        if (rf_regi2c_read(v[0], v[1]) != v[2]) { rf_regi2c_write(v[0], v[1], v[2]); na++; }
    }
    printf("stage +analog (%d regs): %d/10\n", na, run_events(10, 1));
    printf("received %d\n=== done\n", got);
    for (;;) {}
}
