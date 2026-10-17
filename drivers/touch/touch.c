/* Touch sensor v2 (TRM ch. 39): FSM-timer scanning, same defaults as ESP-IDF. */
#include "touch.h"

#define CTRL1      0x60008108   /* b15:0 sleep cycles b31:16 meas num */
#define CTRL2      0x6000810C   /* b3:2 drange b5:4 drefl b7:6 drefh b13 slp_timer b16 start_force b24:17 xpd_wait b28:27 force_done b29 reset b31 clkgate */
#define SCAN_CTRL  0x60008110   /* b2 denoise b8 inactive conn b9 shield b24:10 pad map */
#define DAC        0x6000814C   /* slope pad n (<10) at 29-3n */
#define DAC1       0x60008150   /* slope pad n-10 */
#define TOUCH_CONF 0x6000885C   /* b14:0 outen b17:16 data_sel */
#define STATUS(n)  (0x600088A4 + 4 * ((n) - 1))
#define PAD(n)     (0x60008484 + 4 * (n))   /* b19 mux_sel (RTC) b20 xpd b21 tie b13 fun_ie b27 rue b28 rde */

int touch_init(uint16_t mask)
{
    mask &= 0x7FFE;                                       /* pads 1..14 */
    REG_FIELD(CTRL2, 27, 2, 3); REG_FIELD(CTRL2, 13, 1, 0);   /* stop FSM */
    REG_SET(CTRL2, BIT(29)); REG_CLR(CTRL2, BIT(29));        /* reset */
    REG_FIELD(CTRL1, 16, 16, 500);                         /* charge/discharge cycles */
    REG_FIELD(CTRL1, 0, 16, 0xF);                          /* sleep cycles */
    REG_FIELD(CTRL2, 17, 8, 0xFF);                         /* xpd wait */
    REG_FIELD(CTRL2, 6, 2, 3);                             /* drefh 2.7 V */
    REG_FIELD(CTRL2, 4, 2, 0);                             /* drefl 0.5 V */
    REG_FIELD(CTRL2, 2, 2, 2);                             /* drange 0.5 V */
    REG_FIELD(SCAN_CTRL, 8, 1, 1);                         /* idle pads to GND */
    REG_CLR(SCAN_CTRL, BIT(2) | BIT(9));                   /* no denoise, no shield */
    REG_SET(CTRL2, BIT(31));                               /* clock */
    for (int n = 1; n <= 14; n++) {
        if (!(mask & BIT(n))) continue;
        REG(PAD(n)) = BIT(19);                             /* RTC mux, analog: no IE, no pulls, float */
        if (n < 10) REG_FIELD(DAC, 29 - 3 * n, 3, 7);
        else        REG_FIELD(DAC1, 29 - 3 * (n - 10), 3, 7);
    }
    REG_FIELD(SCAN_CTRL, 10, 15, (REG(SCAN_CTRL) >> 10 & 0x7FFF) | mask);
    REG_FIELD(TOUCH_CONF, 0, 15, (REG(TOUCH_CONF) & 0x7FFF) | mask);
    REG_FIELD(TOUCH_CONF, 16, 2, 0);                       /* read raw data */
    REG_FIELD(CTRL2, 16, 1, 0);                            /* FSM driven by timer */
    REG_FIELD(CTRL2, 27, 2, 0);
    REG_SET(CTRL2, BIT(13));                               /* start the scan timer */
    return 0;
}

uint32_t touch_read(int pad) { return REG(STATUS(pad)) & 0x3FFFFF; }
