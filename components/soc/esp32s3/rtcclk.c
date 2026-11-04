/* RTC clocks, ESP-IDF sequences: rtc_clk_cal_internal (timer group 0 calibration), clk_ll_rtc_slow_set_src,
 * rtc_clk_32k_enable (pads to RTC function, DAC/DRES/DGM 3, DBUF 1, XPD), xt_wdt (32 kHz crystal watchdog). */
#include "rtcclk.h"
#include "clk.h"
#include "sys.h"
#include "cpu.h"

#define CLK_CONF    0x60008074u   /* b31:30 slow src, b29 fast src, b9 8M/256 digi, b8 XTAL32K digi, b7 !8M div, b6 !8M */
#define EXT_XTL     0x60008060u   /* XTAL32K: b23 gpio sel (ext osc), b19:17 dac, b16 xpd, b15:13 dres, b12:10 dgm,
                                     b9 dbuf, b7 xpd force, b4 auto backup, b0 wdt enable */
#define X32P_PAD    0x600084C0u   /* RTC IO XTAL_32P/N pads: b19 RTC mux */
#define X32N_PAD    0x600084C4u
#define CALI_CFG    0x6001F068u   /* TIMG0: b31 start, b30:16 max, b15 rdy, b14:13 clk sel, b12 start cycling */
#define CALI_CFG1   0x6001F06Cu   /* b31:7 XTAL cycles */
#define CALI_CFG2   0x6001F080u   /* b0 timeout, b31:7 timeout threshold */

static uint32_t hz_cache;

uint32_t slow_clk_cal_hz(int src, uint32_t cycles)
{
    static const uint8_t sel[3] = { 0, 2, 1 };                        /* RTC mux (RC), 32K XTAL, 8MD256 */
    if ((unsigned)src >= sizeof sel) return 0;
    periph_enable(PERIPH_TIMG0);
    uint32_t conf = REG(CLK_CONF);
    if (src == SLOW_XTAL32K) REG_SET(CLK_CONF, BIT(8));
    if (src == SLOW_RC8M_D256) REG(CLK_CONF) = (conf & ~(BIT(6) | BIT(7))) | BIT(9);
    if (src == SLOW_RC && slow_clk_src() != SLOW_RC) { REG(CLK_CONF) = conf; return 0; }   /* RC only via the mux */
    if (REG(CALI_CFG) & BIT(12)) {                                    /* stop the ROM's cycling calibration */
        REG(CALI_CFG2) = (REG(CALI_CFG2) & 0x7Fu) | 1u << 7;
        for (uint32_t t = ccount(); !(REG(CALI_CFG) & BIT(15)) && !(REG(CALI_CFG2) & 1) && ccount() - t < 2400000; ) {}
    }
    REG(CALI_CFG) = (REG(CALI_CFG) & ~(BIT(31) | (0x7FFFu << 16) | (3u << 13) | BIT(12))) | (cycles & 0x7FFF) << 16 | (uint32_t)sel[src] << 13;
    REG(CALI_CFG2) = (REG(CALI_CFG2) & 0x7Fu) | 0x1FFFFFFu << 7;
    REG_SET(CALI_CFG, BIT(31));
    uint32_t v = 0, lim = (uint32_t)((uint64_t)cycles * 2 * sys_cpu_hz() / 30000);   /* up to 2x the 32 kHz time */
    for (uint32_t t = ccount(); ccount() - t < lim; ) if (REG(CALI_CFG) & BIT(15)) { v = REG(CALI_CFG1) >> 7; break; }
    REG_CLR(CALI_CFG, BIT(31));
    REG(CLK_CONF) = (REG(CLK_CONF) & ~(BIT(6) | BIT(7) | BIT(8) | BIT(9))) | (conf & (BIT(6) | BIT(7) | BIT(8) | BIT(9)));
    return v ? (uint32_t)(40000000ull * cycles / v) : 0;
}

int slow_clk_src(void) { return (REG(CLK_CONF) >> 30) & 3; }

uint32_t rtc_slow_hz(void)
{
    if (!hz_cache) hz_cache = slow_clk_cal_hz(slow_clk_src() == SLOW_RC ? SLOW_RC : slow_clk_src(), 256);
    return hz_cache;
}

int slow_clk_select(int src)
{
    if (src < 0 || src > 2) return -1;
    if (src == SLOW_RC8M_D256) REG_CLR(CLK_CONF, BIT(6) | BIT(7));   /* 8 MHz RC and its /256 on */
    if (src != SLOW_RC && !slow_clk_cal_hz(src, 64)) return -1;       /* it must run before we switch */
    REG(CLK_CONF) = (REG(CLK_CONF) & ~(3u << 30)) | (uint32_t)src << 30;
    if (src == SLOW_XTAL32K) REG_SET(CLK_CONF, BIT(8));               /* digital side too (RTC timer) */
    hz_cache = 0;
    return 0;
}

int xtal32k_start(bool external_osc)
{
    if (external_osc) {
        REG(0x60009004u + 4 * 15) |= BIT(9);                          /* GPIO15 input */
        REG_SET(EXT_XTL, BIT(23));
    } else {
        REG_SET(X32P_PAD, BIT(19)); REG_SET(X32N_PAD, BIT(19));       /* pads to the crystal */
        REG(EXT_XTL) = (REG(EXT_XTL) & ~(BIT(23) | (7u << 17) | (7u << 13) | (7u << 10) | BIT(9)))
                     | 3u << 17 | 3u << 13 | 3u << 10 | BIT(9) | BIT(16);
    }
    uint32_t t = ccount();
    while (ccount() - t < 400u * (sys_cpu_hz() / 1000)) {             /* a crystal starts in ~100-400 ms */
        if (slow_clk_cal_hz(SLOW_XTAL32K, 32) > 30000) return 0;
        delay_ms(10);
    }
    return -1;
}

void xtal32k_wdt(bool on)
{
    if (on) REG_SET(EXT_XTL, BIT(4) | BIT(0)); else REG_CLR(EXT_XTL, BIT(4) | BIT(0));
}

void rtc_fast_clk_rc(bool rc) { if (rc) REG_SET(CLK_CONF, BIT(29)); else REG_CLR(CLK_CONF, BIT(29)); }
