/* Deep sleep, ESP-IDF sequence for the S3 (esp_hw_support/port/esp32s3/rtc_sleep.c rtc_sleep_low_init,
 * rtc_sleep_init with rtc_sleep_get_default_config(deep), rtc_sleep_start; hal rtc_cntl_ll.h wake sources). RTC fast
 * and slow memory stay powered (the warm-boot stage and RTC_DATA live there). */
#include "sleep.h"
#include "rtcclk.h"
#include "rtcio.h"
#include "regi2c.h"
#include "xtensa.h"
#include "clk.h"
#include "cache.h"
#include "xcore.h"
#include "sys.h"

#define R(o)            (0x60008000u + (o))
#define OPTIONS0        R(0x00)
#define SLP_TIMER0      R(0x04)
#define SLP_TIMER1      R(0x08)   /* b16 alarm enable */
#define TIME_UPDATE     R(0x0C)
#define TIME_LO         R(0x10)
#define TIME_HI         R(0x14)
#define STATE0          R(0x18)   /* b31 sleep, b1 reject cause clear */
#define TIMER1          R(0x1C)   /* b31:24 PLL buf wait, b23:14 XTL buf wait, b13:6 8M wait */
#define TIMER2          R(0x20)   /* b23:15 touch/ULP start wait */
#define ANA_CONF        R(0x34)
#define WAKEUP_STATE    R(0x3C)   /* b31:15 wake enable */
#define INT_RAW         R(0x44)
#define INT_CLR         R(0x4C)
#define EXT_WAKEUP_CONF R(0x64)   /* b30 EXT0 level, b31 EXT1 level */
#define SLP_REJECT      R(0x68)   /* b31 deep reject en, b30 light reject en, b29:12 reject enable */
#define CLK_CONF        R(0x74)
#define SDIO_CONF       R(0x7C)
#define BIAS_CONF       R(0x80)
#define RTC_REG         R(0x84)
#define PWC             R(0x88)
#define DIG_PWC         R(0x90)
#define DIG_ISO         R(0x94)
#define EXT_WAKEUP1     R(0xE0)   /* b21:0 pins, b22 status clear */
#define EXT_WAKEUP1_ST  R(0xE4)
#define WAKEUP_CAUSE    R(0x130)
#define REG_DRV         R(0x8C)   /* b19:12 DG_VDD_DRV_B_SLP */
#define EXT_WAKEUP0     0x600084DCu   /* RTC IO: b31:27 EXT0 pin */
#define USJ_CONF0       0x60038018u   /* b14 USB pad enable */
#define PERIP_CLK_EN1   0x600C001Cu   /* b2 USB_DEVICE (Serial/JTAG) clock */
#define FIELD(reg, hi, lo, v) (REG(reg) = (REG(reg) & ~(((1u << ((hi) - (lo) + 1)) - 1) << (lo))) | ((uint32_t)(v) << (lo)))

static uint32_t triggers;
static uint64_t timer_us;

void sleep_wake_timer(uint64_t us) { timer_us = us; triggers |= WAKE_TIMER; }

int sleep_wake_ext0(int pin, int level)
{
    if (!(pin >= 0 && pin <= 21)) return -1;      /* caller index: never past the hardware */
    if (rtcio_config(pin, GPIO_INPUT) < 0) return -1;
    FIELD(EXT_WAKEUP0, 31, 27, pin);
    FIELD(EXT_WAKEUP_CONF, 30, 30, level ? 1 : 0);
    triggers |= WAKE_EXT0;
    return 0;
}

int sleep_wake_ext1(uint32_t mask, bool any_high)
{
    if (mask >> 22) return -1;
    for (int p = 0; p < 22; p++) if (mask & BIT(p)) rtcio_config(p, GPIO_INPUT);
    FIELD(EXT_WAKEUP1, 21, 0, mask);
    FIELD(EXT_WAKEUP_CONF, 31, 31, any_high ? 1 : 0);
    REG_SET(EXT_WAKEUP1, BIT(22));                                    /* status clear */
    triggers |= WAKE_EXT1;
    return 0;
}

void sleep_wake_none(void) { triggers = 0; timer_us = 0; }
uint32_t sleep_wake_cause(void) { return REG(WAKEUP_CAUSE) & 0x1FFFF; }
uint32_t sleep_ext1_pins(void) { return REG(EXT_WAKEUP1_ST) & 0x3FFFFF; }

static uint64_t rtc_ticks(void)
{
    REG(TIME_UPDATE) = BIT(31); for (volatile int k = 0; k < 200; k++) {}
    return (uint64_t)REG(TIME_HI) << 32 | REG(TIME_LO);
}
uint64_t sleep_rtc_us(void) { return rtc_ticks() * 1000000u / rtc_slow_hz(); }

static void arm_timer(void)
{
    if (!(triggers & WAKE_TIMER)) return;
    uint64_t t = rtc_ticks() + timer_us * rtc_slow_hz() / 1000000u;
    REG(SLP_TIMER0) = (uint32_t)t; REG(SLP_TIMER1) = (uint32_t)(t >> 32) & 0xFFFF;
    REG(INT_CLR) = BIT(10);
    REG_SET(SLP_TIMER1, BIT(16));
}

/* Light sleep, IDF esp_sleep_start(LIGHT) with rtc_sleep_get_default_config for XTAL kept on (digital 1.1 V, no
 * drop): RAM, CPU state, peripherals and flash stay powered; XTAL, BBPLL and RC8M are forced on so the wake needs
 * no PLL relock (ponytail: lowest-risk bring-up, not lowest current; drop them as IDF does once measured). USB
 * Serial/JTAG pad + clock off over the sleep as IDF (the host sees a re-plug), core 1 stalled, cache suspended. */
RAM_FN static uint32_t light_core(void)
{
    REG(WAKEUP_STATE) = (REG(WAKEUP_STATE) & 0x7FFF) | triggers << 15;
    FIELD(SLP_REJECT, 29, 12, 0);
    REG(INT_CLR) = BIT(0) | BIT(1);
    REG_SET(STATE0, BIT(1));
    REG_SET(STATE0, BIT(31));
    uint32_t t = 0;
    while (!(REG(INT_RAW) & (BIT(0) | BIT(1)))) if (++t == 0) break;          /* wake or reject */
    uint32_t reject = REG(INT_RAW) & BIT(1);
    REG(INT_CLR) = BIT(0) | BIT(1);
    FIELD(TIMER2, 23, 15, 0x10);                                              /* touch/ULP wait default */
    return reject ? 0 : 1;
}

uint32_t sleep_light(void)
{
    if (!triggers) return 0;
    uint32_t ps = irq_lock();
    bool c1 = sys_cpu1_running();
    if (c1) xc_stall(1);
    uint32_t mhz = clk_cpu_hz() / 1000000;
    uint32_t opt0 = REG(OPTIONS0);
    REG_SET(OPTIONS0, BIT(11) | BIT(9) | BIT(13));                            /* BBPLL, its I2C, XTAL forced on */
    clk_cpu_set_mhz(40);
    uint32_t usj_pad = REG(USJ_CONF0) & BIT(14), usj_clk = REG(PERIP_CLK_EN1) & BIT(2);
    REG_CLR(USJ_CONF0, BIT(14)); REG_CLR(PERIP_CLK_EN1, BIT(2));
    arm_timer();
    /* rtc_sleep_low_init(light) */
    uint32_t xtl_wait = 250u * rtc_slow_hz() / 1000000u + 1;
    FIELD(TIMER1, 31, 24, 1); FIELD(TIMER1, 23, 14, xtl_wait > 1023 ? 1023 : xtl_wait); FIELD(TIMER1, 13, 6, 4);
    /* rtc_sleep_init(light, XTAL on) */
    REG_CLR(DIG_PWC, BIT(30) | BIT(29) | BIT(28)); REG_CLR(PWC, BIT(20));    /* modem, CPU, digital + RTC periph kept */
    regi2c_write_mask(REGI2C_DIG, 5, 4, 0, 23);                                /* RTC 1.10 V in sleep */
    regi2c_write_mask(REGI2C_DIG, 7, 4, 0, 23);                                /* digital 1.10 V in sleep */
    FIELD(BIAS_CONF, 21, 18, 0); FIELD(BIAS_CONF, 16, 16, 0); FIELD(BIAS_CONF, 14, 14, 0);   /* NODROP, bias + current on */
    FIELD(BIAS_CONF, 25, 22, 0); FIELD(BIAS_CONF, 17, 17, 0); FIELD(BIAS_CONF, 15, 15, 0);
    FIELD(REG_DRV, 19, 12, 0xF); REG_CLR(DIG_PWC, BIT(31));                   /* not deep: digital wrap kept */
    REG_SET(DIG_PWC, BIT(4)); REG_SET(PWC, BIT(8) | BIT(11));
    FIELD(RTC_REG, 31, 31, 1);
    REG_SET(CLK_CONF, BIT(26));                                               /* RC8M on */
    REG_CLR(SDIO_CONF, BIT(22)); FIELD(SDIO_CONF, 21, 21, 0);                 /* flash stays powered */
    REG_SET(SLP_REJECT, BIT(31) | BIT(30));
    FIELD(TIMER2, 23, 15, 0xFF);
    FIELD(OPTIONS0, 13, 13, 1); FIELD(CLK_CONF, 28, 28, 1);                   /* XTAL on, no global gating */
    uint32_t cs = cache_flash_op_begin();
    uint32_t woke = light_core();
    cache_flash_op_end(cs, 0, 0);
    if (usj_clk) REG_SET(PERIP_CLK_EN1, BIT(2));
    if (usj_pad) REG_SET(USJ_CONF0, BIT(14));
    clk_cpu_set_mhz(mhz);
    REG(OPTIONS0) = (REG(OPTIONS0) & ~(BIT(11) | BIT(9) | BIT(13))) | (opt0 & (BIT(11) | BIT(9) | BIT(13)));
    if (c1) xc_unstall(1);
    irq_unlock(ps);
    return woke ? sleep_wake_cause() : 0;
}

NORETURN void sleep_deep(void)
{
    irq_lock();
    arm_timer();
    /* rtc_sleep_low_init(deep): default buffer waits */
    FIELD(TIMER1, 31, 24, 20); FIELD(TIMER1, 23, 14, 100); FIELD(TIMER1, 13, 6, 20);
    /* rtc_sleep_init, deep-sleep defaults (RTC_SLEEP_PD_DIG | VDDSDIO | XTAL, 8M off unless it is the slow clock) */
    bool rc8m = slow_clk_src() == SLOW_RC8M_D256, rtc_io = triggers & (WAKE_EXT0 | WAKE_EXT1);
    REG_CLR(DIG_PWC, BIT(30));                                        /* modem: covered by the wrap power-down */
    REG_CLR(DIG_PWC, BIT(29)); REG_CLR(DIG_PWC, BIT(28));             /* CPU / digital periph: wrap does it */
    if (!rtc_io) { REG_CLR(PWC, BIT(5) | BIT(4) | BIT(19)); REG_SET(PWC, BIT(20)); }   /* RTC periph off */
    else REG_CLR(PWC, BIT(20));
    regi2c_write_mask(REGI2C_DIG, 5, 4, 0, 23);                       /* RTC voltage in sleep: 1.10 V */
    regi2c_write_mask(REGI2C_DIG, 7, 4, 0, 0);
    uint32_t atten = rc8m || rtc_io ? 0 : 14;                         /* NODROP when 8M/RTC IO needed, else default */
    FIELD(BIAS_CONF, 21, 18, atten); FIELD(BIAS_CONF, 16, 16, 1); FIELD(BIAS_CONF, 14, 14, 1);
    FIELD(BIAS_CONF, 25, 22, 0); FIELD(BIAS_CONF, 17, 17, 1); FIELD(BIAS_CONF, 15, 15, 1);
    REG_SET(DIG_PWC, BIT(31));                                        /* digital wrap powered down */
    REG_CLR(ANA_CONF, BIT(30) | BIT(31) | BIT(28) | BIT(27));         /* CKGEN / PLL / RF I2C power */
    REG_CLR(OPTIONS0, BIT(7));
    REG_SET(DIG_PWC, BIT(4)); REG_SET(PWC, BIT(8) | BIT(11));          /* memories: low-power, kept */
    FIELD(RTC_REG, 31, 31, 1);                                        /* RTC regulator on (no ultra-low mode) */
    if (rc8m) REG_SET(CLK_CONF, BIT(26)); else REG_CLR(CLK_CONF, BIT(26));
    REG_CLR(SDIO_CONF, BIT(22)); FIELD(SDIO_CONF, 21, 21, 1);         /* VDD_SDIO off by the state machine */
    REG_SET(SLP_REJECT, BIT(31) | BIT(30));
    FIELD(TIMER2, 23, 15, 0xFF);
    FIELD(OPTIONS0, 13, 13, 0); FIELD(CLK_CONF, 28, 28, 0);           /* XTAL not forced on */
    /* rtc_sleep_start */
    FIELD(WAKEUP_STATE, 31, 15, triggers);
    FIELD(SLP_REJECT, 29, 12, 0);
    REG(INT_CLR) = BIT(0) | BIT(1);
    REG_SET(STATE0, BIT(1));
    REG_SET(STATE0, BIT(31));
    for (;;) {}                                                       /* deep sleep: the wake is a reset */
}
