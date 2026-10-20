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

#define R32(a)  (*(volatile uint32_t *)(a))
#define CORE(o) R32(0x60031000u + (o))
static uint32_t em_buf[BLE_EM_WORDS];

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
    for (int k = 0; k < 64; k++) R32(0x60011000u + 4 * k) = ble_modem_regs[k];
    memcpy(em_buf, ble_em_image, sizeof em_buf);
    for (unsigned k = 0; k < sizeof ble_em_segs / sizeof ble_em_segs[0]; k++) {
        unsigned i = ble_em_segs[k].idx;
        uint32_t a = (uint32_t)em_buf + ble_em_segs[k].pos;
        R32(0x60031204u + 4 * i + (i > 47 ? 28 : 0)) = ((uint32_t)ble_em_segs[k].em_off >> 2) << 18 | ((a >> 2) & 0x3FFFF);
    }
    for (unsigned k = 0; k < sizeof ble_core_regs / sizeof ble_core_regs[0]; k++) CORE(ble_core_regs[k].off) = ble_core_regs[k].val;
    CORE(0x0C) = 0;                                 /* polled */
    CORE(0x00) = ble_rwblecntl;
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
    rf_rx_on();
    ble_setup();
    printf("core ver %08lx clkn %07lx\n", (unsigned long)CORE(0x04), (unsigned long)clkn());
    uint8_t n0 = rf_regi2c_read(0x63, 3), f0 = rf_regi2c_read(0x63, 4);
    uint32_t b0 = R32(0x600110B0);
    printf("stage base: %d/10\n", run_events(10, 1));
    printf("  modem b0 %08lx -> %08lx, pll N %02x.%02x -> %02x.%02x, 6c %08lx 70 %08lx 7c %08lx 80 %08lx\n", (unsigned long)b0, (unsigned long)R32(0x600110B0),
           n0, f0, rf_regi2c_read(0x63, 3), rf_regi2c_read(0x63, 4), (unsigned long)R32(0x6001106C), (unsigned long)R32(0x60011070),
           (unsigned long)R32(0x6001107C), (unsigned long)R32(0x60011080));
    const unsigned nb = sizeof ble_vendor_phy_blocks / sizeof ble_vendor_phy_blocks[0];
    for (unsigned b = 0; b < nb; b++) {
        int nch = 0;
        for (unsigned k = 0; k < sizeof ble_vendor_phy / sizeof ble_vendor_phy[0]; k++) {
            uint32_t a = ble_vendor_phy[k].addr;
            if (a < ble_vendor_phy_blocks[b] || (b + 1 < nb && a >= ble_vendor_phy_blocks[b + 1] && ble_vendor_phy_blocks[b + 1] > ble_vendor_phy_blocks[b])) continue;
            if ((a & ~0xFFFu) != (ble_vendor_phy_blocks[b] & ~0xFFFu) && (a & ~0x1FFFu) != ble_vendor_phy_blocks[b]) continue;
            if (R32(a) != ble_vendor_phy[k].val) { R32(a) = ble_vendor_phy[k].val; nch++; }
        }
        printf("stage +%08lx (%d regs): %d/10\n", (unsigned long)ble_vendor_phy_blocks[b], nch, run_events(10, 1));
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
