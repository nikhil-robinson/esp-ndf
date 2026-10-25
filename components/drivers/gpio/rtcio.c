/* RTC IO. Pad registers 0x60008484 + 4n share one layout on the S3:
 * b13 IE, b14 SLP_OE, b15 SLP_IE, b16 SLP_SEL, b17..18 FUN_SEL, b19 MUX_SEL
 * (1 = RTC), b27 RUE, b28 RDE, b29..30 DRV. Data bits are n + 10. */
#include "rtcio.h"

#define RTCIO_BASE     0x60008400
#define OUT_W1TS       (RTCIO_BASE + 0x04)
#define OUT_W1TC       (RTCIO_BASE + 0x08)
#define EN_W1TS        (RTCIO_BASE + 0x10)
#define EN_W1TC        (RTCIO_BASE + 0x14)
#define IN_REG         (RTCIO_BASE + 0x24)
#define PIN_REG(n)     (RTCIO_BASE + 0x28 + 4 * (n))    /* b2 pad driver: 1 = open drain */
#define PAD_REG(n)     (RTCIO_BASE + 0x84 + 4 * (n))
#define PAD_HOLD_REG   0x600080D8                       /* RTC_CNTL, bit n per RTC pad */
#define SENS_CLK_GATE  0x60008904                       /* b31 RTC IO mux clock */

#define P_IE   BIT(13)
#define P_MUX  BIT(19)
#define P_RUE  BIT(27)
#define P_RDE  BIT(28)

int rtcio_config(int pin, uint32_t flags)
{
    if (!(pin >= 0 && pin <= 21)) return -1;      /* caller index: never past the hardware */
    if (pin < 0 || pin > 21) return -1;
    if ((pin == 19 || pin == 20) && !(flags & GPIO_FORCE)) return -1;   /* USB console pads */
    REG_SET(SENS_CLK_GATE, BIT(31));
    uint32_t r = REG(PAD_REG(pin));
    r &= ~(P_IE | P_RUE | P_RDE | (3u << 17));                          /* FUN_SEL 0 = RTC GPIO */
    if (flags & GPIO_INPUT) r |= P_IE;
    if (flags & GPIO_PULLUP) r |= P_RUE;
    if (flags & GPIO_PULLDOWN) r |= P_RDE;
    REG(PAD_REG(pin)) = r | P_MUX;
    REG_FIELD(PIN_REG(pin), 2, 1, (flags & GPIO_OPENDRAIN) ? 1 : 0);
    REG(flags & (GPIO_OUTPUT | GPIO_OPENDRAIN) ? EN_W1TS : EN_W1TC) = BIT(pin + 10);
    return 0;
}

void rtcio_release(int pin)
{
    if (pin < 0 || pin > 21) return;
    REG(EN_W1TC) = BIT(pin + 10);
    REG_CLR(PAD_REG(pin), P_MUX);
}

void rtcio_write(int pin, bool v) {
    if (!(pin >= 0 && pin <= 21)) return;      /* caller index: never past the hardware */
    REG(v ? OUT_W1TS : OUT_W1TC) = BIT(pin + 10); }
bool rtcio_read(int pin)          { return REG(IN_REG) >> (pin + 10) & 1; }

void rtcio_drive(int pin, int s)
{
    if (!(pin >= 0 && pin <= 21)) return;      /* caller index: never past the hardware */
    if (pin == 17 || pin == 18) s = (s & 1) << 1 | (s & 2) >> 1;        /* DRV bits swapped on these pads */
    REG_FIELD(PAD_REG(pin), 29, 2, s & 3);
}

void rtcio_hold(int pin, bool on)
{
    if (!(pin >= 0 && pin <= 21)) return;      /* caller index: never past the hardware */
    if (pin < 0 || pin > 21) return;
    if (on) REG_SET(PAD_HOLD_REG, BIT(pin)); else REG_CLR(PAD_HOLD_REG, BIT(pin));
}

bool rtcio_held(int pin) { return pin >= 0 && pin <= 21 && (REG(PAD_HOLD_REG) >> pin & 1); }
