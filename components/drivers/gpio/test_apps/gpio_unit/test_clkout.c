#include "test.h"
#include "clk.h"
#include "pcnt.h"
#include "rtcclk.h"
#include "sys.h"

/* CLK_OUT1 on GPIO41: RC slow (~136 kHz, compared with its calibration) and RC fast (~17.5 MHz), counted by PCNT
 * on the same pad (XTAL measured 40.03 MHz the same way; PLL 80 MHz is beyond PCNT through the matrix). */
static int32_t count(uint32_t us)
{
    pcnt_channel(0, 0, 41, -1, PCNT_EDGE_UP, PCNT_EDGE_OFF, PCNT_CTRL_KEEP, PCNT_CTRL_KEEP);
    pcnt_clear(0); delay_us(us); return pcnt_get(0);
}

void test_clkout(void)
{
    CHECK(clk_out(1, CLK_OUT_XTAL, 20) == -1);              /* the USB pin is refused */
    CHECK(clk_out(1, CLK_OUT_RC_SLOW, 41) == 0);
    int32_t s = count(100000);
    uint32_t cal = rtc_slow_hz();
    CHECK(clk_out(1, CLK_OUT_RC_FAST, 41) == 0);
    int32_t f = count(1000);
    clk_out(1, -1, 41);
    printf("    CLK_OUT1 GPIO41: RC slow %ld Hz (calibrated %lu Hz), RC fast %ld kHz\n", s * 10, cal, f);
    CHECK(s * 10 > (int32_t)cal * 9 / 10 && s * 10 < (int32_t)cal * 11 / 10 && f > 14000 && f < 21000);
}

NDF_UNIT(030, clkout);
