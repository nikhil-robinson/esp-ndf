/* P4 sleep (sleep.h): light sleep and deep sleep through the PMU, wake by the LP timer or EXT1 (LP IO 0..7).
 * Flow of ESP-IDF v6.1 esp_hw_support/port/esp32p4/pmu_sleep.c (pmu_sleep_config_default / pmu_sleep_init /
 * pmu_sleep_start) and sleep_modes.c. The register values are not typed in: tools/p4_pmu/gen.c runs IDF's own PMU code
 * on the host and writes pmu_img.h (the sleep-mode words of the PMU for deep and for light sleep).
 * Light sleep: nothing is powered down (RAM, CPU, peripherals kept; the PMU only gates clocks and drops the PLLs), so
 * the CPU runs from the 40 MHz crystal around it, PSRAM goes to a safe state, and the CPU frequency is restored after.
 * Deep sleep: HP top domain, CPU, crystal and RC_FAST off; LP RAM and the LP peripherals stay; the wake is a reset
 * (cause RESET_DEEPSLEEP) and the ROM boots the image again. NOT VERIFIED ON HARDWARE (docs/P4.md). */
#include "sleep.h"
#include "rtcclk.h"
#include "rtcio.h"
#include "cache.h"
#include "clk.h"
#include "cpu.h"
#include "psram.h"
#include "sys.h"
#include "xcore.h"
#include "soc.h"
#include "pmu_img.h"

#define LP_TIMER          0x50112000u
#define LT_TAR0_LOW       (LP_TIMER + 0x00)
#define LT_TAR0_HIGH      (LP_TIMER + 0x04)               /* b15:0 high word, b31 enable */
#define LT_UPDATE         (LP_TIMER + 0x10)               /* b28 snapshot */
#define LT_BUF0_LOW       (LP_TIMER + 0x14)
#define LT_BUF0_HIGH      (LP_TIMER + 0x18)               /* b15:0 */
#define LT_INT_CLR        (LP_TIMER + 0x34)               /* b31 timer 0 wake */
#define PMU_EXT_LV        (PMU_BASE + 0x1E8)
#define PMU_EXT_SEL       (PMU_BASE + 0x1EC)
#define PMU_EXT_ST        (PMU_BASE + 0x1F0)
#define PMU_EXT_CNTL      (PMU_BASE + 0x1F4)              /* b30 status clear */
#define LP_SYS_STORE8     (LPAON + 0x4C)                  /* RTC_SLEEP_MODE_REG: b0 deep sleep (the ROM runs the wake stub) */
#define PAD_SLP_SEL       (PMU_BASE + 0xE4)               /* imm.pad_hold_all: b26 pads to the sleep state, b27 back to active */
#define PMU_WAKE_GPIO     BIT(2)
#define PMU_WAKE_EXT1     BIT(12)
#define PMU_WAKE_TIMER    BIT(13)
#define L1D               BIT(4)
#define ROM_WRITEBACK_ALL(m) ((int (*)(uint32_t))0x4fc00414)(m)

static uint32_t triggers, ext1_mask, ext1_lv;
static uint64_t timer_us;

void sleep_wake_timer(uint64_t us) { timer_us = us; triggers |= WAKE_TIMER; }

int sleep_wake_ext0(int pin, int level)
{
    if (!(pin >= 0 && pin <= 7)) return -1;               /* the P4 has EXT1 only: one pin of it plays EXT0 */
    if (rtcio_config(pin, GPIO_INPUT) < 0) return -1;
    ext1_mask |= BIT(pin);
    ext1_lv = level ? ext1_lv | BIT(pin) : ext1_lv & ~BIT(pin);
    triggers |= WAKE_EXT0;
    return 0;
}

int sleep_wake_ext1(uint32_t mask, bool any_high)
{
    if (mask >> 8) return -1;
    for (int p = 0; p < 8; p++) if (mask & BIT(p)) rtcio_config(p, GPIO_INPUT);
    ext1_mask |= mask;
    ext1_lv = any_high ? ext1_lv | mask : ext1_lv & ~mask;
    triggers |= WAKE_EXT1;
    return 0;
}

void sleep_wake_none(void) { triggers = 0; timer_us = 0; ext1_mask = ext1_lv = 0; }

static uint64_t rtc_ticks(void)
{
    REG(LT_UPDATE) = BIT(28);
    for (volatile int k = 0; k < 200; k++) {}
    return (uint64_t)(REG(LT_BUF0_HIGH) & 0xFFFF) << 32 | REG(LT_BUF0_LOW);
}
uint64_t sleep_rtc_us(void) { return rtc_ticks() * 1000000u / rtc_slow_hz(); }

uint32_t sleep_wake_cause(void)
{
    uint32_t w = REG(PMU_BASE + PMU_WAKEUP_STATUS0_OFF), c = 0;
    if (w & PMU_WAKE_TIMER) c |= WAKE_TIMER;
    if (w & PMU_WAKE_EXT1) c |= ext1_mask && (triggers & WAKE_EXT0) && !(triggers & WAKE_EXT1) ? WAKE_EXT0 : WAKE_EXT1;
    if (w & PMU_WAKE_GPIO) c |= WAKE_GPIO;
    return c;
}
uint32_t sleep_ext1_pins(void) { return REG(PMU_EXT_ST) & 0xFF; }

static uint32_t pmu_wake_bits(void)
{
    return (triggers & WAKE_TIMER ? PMU_WAKE_TIMER : 0) | (triggers & (WAKE_EXT0 | WAKE_EXT1) ? PMU_WAKE_EXT1 : 0) | (triggers & WAKE_GPIO ? PMU_WAKE_GPIO : 0);
}

static void arm_wake_sources(void)
{
    if (triggers & WAKE_TIMER) {
        uint64_t t = rtc_ticks() + timer_us * rtc_slow_hz() / 1000000u;
        REG(LT_TAR0_LOW) = (uint32_t)t;
        REG(LT_INT_CLR) = BIT(31);
        REG(LT_TAR0_HIGH) = ((uint32_t)(t >> 32) & 0xFFFF) | BIT(31);
    }
    if (triggers & (WAKE_EXT0 | WAKE_EXT1)) {
        REG(PMU_EXT_SEL) = ext1_mask;
        REG(PMU_EXT_LV) = ext1_lv;
        REG_SET(PMU_EXT_CNTL, BIT(30)); REG_CLR(PMU_EXT_CNTL, BIT(30));
    }
}

static void apply(const pmu_img_t *img, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        volatile uint32_t *r = (volatile uint32_t *)(PMU_BASE + img[i].off);
        *r = (*r & ~img[i].mask) | img[i].val;
    }
}

/* pmu_sleep_start: wake / reject sources, clear status, write back the data cache, request sleep, wait for the PMU to
 * wake the HP system or to reject the request. Returns true when the sleep was rejected. */
RAM_FN static bool pmu_sleep_go(uint32_t wake, bool dslp)
{
    if (dslp) REG_SET(LP_SYS_STORE8, BIT(0)); else REG_CLR(LP_SYS_STORE8, BIT(0));
    REG(PMU_BASE + PMU_WAKEUP_CNTL2_OFF) = wake;
    REG(PMU_BASE + PMU_WAKEUP_CNTL1_OFF) = (wake & ~BIT(0)) | PMU_SLP_REJECT_EN;      /* reject if a wake source is already up */
    REG(PMU_BASE + PMU_INT_CLR_OFF) = PMU_INT_CLR_WAKEUP | PMU_INT_CLR_REJECT;
    REG_SET(PMU_BASE + PMU_WAKEUP_CNTL4_OFF, PMU_REJECT_CAUSE_CLR);
    ROM_WRITEBACK_ALL(L1D);                                                           /* no dirty data after this point */
    REG(PAD_SLP_SEL) = BIT(26);                                                       /* pads into their sleep state */
    REG_SET(PMU_BASE + PMU_WAKEUP_CNTL0_OFF, PMU_SLEEP_REQ);
    while (!(REG(PMU_BASE + PMU_INT_RAW_OFF) & (PMU_INT_RAW_WAKEUP | PMU_INT_RAW_REJECT))) {}
    REG(PAD_SLP_SEL) = BIT(27);
    return REG(PMU_BASE + PMU_INT_RAW_OFF) & PMU_INT_RAW_REJECT;
}

uint32_t sleep_light(void)
{
    if (!triggers) return 0;
    uint32_t mhz = clk_cpu_hz() / 1000000, cs = cache_flash_op_begin();
    clk_cpu_set_mhz(40);                                                              /* the PLLs go off in sleep */
    bool ps = psram_size() != 0;
    if (ps) psram_sleep_enter();
    arm_wake_sources();
    apply(pmu_img_light, sizeof pmu_img_light / sizeof pmu_img_light[0]);
    REG_FIELD(PMU_BASE + PMU_HP_ACT_BIAS_OFF, 23, 2, 1);                              /* HP ACTIVE dcm_mode: 1 around light sleep */
    bool rejected = pmu_sleep_go(pmu_wake_bits(), false);
    if (ps) psram_sleep_exit();
    clk_cpu_set_mhz(mhz);
    cache_flash_op_end(cs, 0, 0);
    return rejected ? 0 : sleep_wake_cause();
}

NORETURN void sleep_deep(void)
{
    irq_lock();
    arm_wake_sources();
    apply(pmu_img_deep, sizeof pmu_img_deep / sizeof pmu_img_deep[0]);
    REG_FIELD(PMU_BASE + PMU_HP_ACT_BIAS_OFF, 23, 2, 0);                              /* DCDC mode off for the deep sleep */
    pmu_sleep_go(pmu_wake_bits(), true);
    for (;;) {}                                                                       /* the wake is a reset */
}
