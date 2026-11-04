/* RTC slow clock, same API as soc/esp32s3/rtcclk.c. ponytail: only the internal RC_SLOW (power-on default, nominal
 * 136 kHz, IDF SOC_CLK_RC_SLOW_FREQ_APPROX) and no calibration yet; the 32 kHz crystal and calibration through
 * TIMG come with the P4 sleep / RTC work. */
#include "rtcclk.h"

int      slow_clk_select(int src) { return src == SLOW_RC ? 0 : -1; }
int      slow_clk_src(void) { return SLOW_RC; }
uint32_t slow_clk_cal_hz(int src, uint32_t cycles) { (void)cycles; return src == SLOW_RC ? 136000u : 0; }
uint32_t rtc_slow_hz(void) { return 136000u; }
int      xtal32k_start(bool external_osc) { (void)external_osc; return -1; }
void     xtal32k_wdt(bool on) { (void)on; }
void     rtc_fast_clk_rc(bool rc) { (void)rc; }
