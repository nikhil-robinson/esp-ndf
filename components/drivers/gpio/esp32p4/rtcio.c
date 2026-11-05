/* P4 RTC IO (rtcio.h): the LP IO block drives GPIO0..15 (rtcio n = gpio n). Registers from ESP-IDF v6.1 hw_ver3 lp_gpio_reg.h /
 * lp_iomux_reg.h (offsets computed from the struct headers), flow of esp_hal_gpio/esp32p4 rtc_io_ll.h: pad to the LP
 * mux (mux_sel 1, function 1 = LP_GPIO), direction through the LP_GPIO enable set / clear registers, hold through LP_SYS
 * pad_rtc_hold_ctrl0 (rev >= 3). */
#include "rtcio.h"
#include "soc.h"

#define LP_GPIO_BASE   0x5012A000u
#define LP_IOMUX_BASE  0x5012B000u
#define LP_GPIO_CLK_EN (LP_GPIO_BASE + 0x00)
#define OUT_W1TS       (LP_GPIO_BASE + 0x0C)
#define OUT_W1TC       (LP_GPIO_BASE + 0x10)
#define EN_W1TS        (LP_GPIO_BASE + 0x18)
#define EN_W1TC        (LP_GPIO_BASE + 0x1C)
#define IN_REG         (LP_GPIO_BASE + 0x30)             /* b15:0 */
#define PIN_REG(n)     (LP_GPIO_BASE + 0x34 + 4 * (n))   /* b0 wakeup enable, b3:1 int type, b4 open drain, b5 edge wakeup clear */
#define OUT_SEL(n)     (LP_GPIO_BASE + 0xF4 + 4 * (n))   /* b8:3 output signal */
#define LP_SIG_GPIO_OUT 32
#define PAD_REG(n)     (LP_IOMUX_BASE + 0x08 + 4 * (n))  /* b1:0 drv b2 rde b3 rue b4 mux_sel b6:5 fun_sel b10 ie b7 slp_sel b8 slp_ie b9 slp_oe */
#define PAD_HOLD       (LPAON + 0x1C4)                   /* LP_SYS pad_rtc_hold_ctrl0, bit n per pad (rev >= 3) */

#define P_DRV   0x3u
#define P_RDE   BIT(2)
#define P_RUE   BIT(3)
#define P_MUX   BIT(4)
#define P_FUN   (3u << 5)
#define P_IE    BIT(10)

int rtcio_config(int pin, uint32_t flags)
{
    if (!(pin >= 0 && pin <= 15)) return -1;           /* caller index: never past the hardware */
    REG_SET(LP_GPIO_CLK_EN, BIT(0));
    uint32_t r = REG(PAD_REG(pin));
    r &= ~(P_IE | P_RUE | P_RDE | P_FUN);
    if (flags & GPIO_INPUT) r |= P_IE;
    if (flags & GPIO_PULLUP) r |= P_RUE;
    if (flags & GPIO_PULLDOWN) r |= P_RDE;
    REG(PAD_REG(pin)) = r | P_MUX | 1u << 5;           /* LP mux, function 1 = LP_GPIO */
    REG_FIELD(OUT_SEL(pin), 3, 6, LP_SIG_GPIO_OUT);
    REG_FIELD(PIN_REG(pin), 4, 1, (flags & GPIO_OPENDRAIN) ? 1 : 0);
    REG(flags & (GPIO_OUTPUT | GPIO_OPENDRAIN) ? EN_W1TS : EN_W1TC) = BIT(pin);
    return 0;
}

void rtcio_release(int pin)
{
    if (pin < 0 || pin > 15) return;
    REG(EN_W1TC) = BIT(pin);
    REG_CLR(PAD_REG(pin), P_MUX);
}

void rtcio_write(int pin, bool v)
{
    if (!(pin >= 0 && pin <= 15)) return;
    REG(v ? OUT_W1TS : OUT_W1TC) = BIT(pin);
}
bool rtcio_read(int pin) { return pin >= 0 && pin <= 15 && (REG(IN_REG) >> pin & 1); }

void rtcio_drive(int pin, int s)
{
    if (!(pin >= 0 && pin <= 15)) return;
    REG_FIELD(PAD_REG(pin), 0, 2, s & 3);
}

void rtcio_hold(int pin, bool on)
{
    if (!(pin >= 0 && pin <= 15)) return;
    if (on) REG_SET(PAD_HOLD, BIT(pin)); else REG_CLR(PAD_HOLD, BIT(pin));
}

bool rtcio_held(int pin) { return pin >= 0 && pin <= 15 && (REG(PAD_HOLD) >> pin & 1); }
