/* P4 LP core as the ULP coprocessor (ulp.h): the program is a raw binary linked at LP RAM (0x50108000, reset handler at
 * +0x80 after the 0x80-byte vector table, ESP-IDF v6.1 ulp/lp_core/lp_core.c RESET_HANDLER_ADDR), at most NDF_ULP_RESERVE
 * bytes. Boots straight into the application (skip_lp_rom_boot). ulp_run(period_us): the LP timer 1 wakes the core;
 * the LP timer alarm is one-shot, so the program re-arms it with the tick count kept in the notes words just above the
 * program (ulp_period_ticks): LP code adds it to LP_TIMER's counter and writes LP_TIMER_TAR1 (ulp_lp_core_lp_timer_
 * set_wakeup_ticks does the same). Registers: hw_ver3 lpperi / lp_system / pmu structs (offsets computed from them).
 * ponytail: the LP core debug module is left available and the LP UART is not touched. */
#include "ulp.h"
#include "rtcclk.h"
#include "sys.h"
#include "soc.h"

#define LPPERI_BASE     0x50120000u
#define LPPERI_CLK_EN   (LPPERI_BASE + 0x00)       /* b31 LP core bus clock */
#define LPPERI_RESET_EN (LPPERI_BASE + 0x08)       /* b31 LP core reset */
#define LPPERI_CPU      (LPPERI_BASE + 0x0C)       /* b31 debug module unavailable */
#define LP_SYS_CORE_BOOT (LPAON + 0x18)            /* LP core reset vector */
#define LP_SYS_APP_BOOT  (LPAON + 0x28)            /* boot_addr_hp_lp (where the LP ROM would jump) */
#define PMU_LP_PWR0     (PMU_BASE + 0x184)         /* b30 reset at sleep, b29 stall at sleep */
#define PMU_LP_PWR1     (PMU_BASE + 0x188)         /* b31 sleep request */
#define PMU_LP_PWR2     (PMU_BASE + 0x18C)         /* wake sources of the LP core */
#define PMU_HP_LP_COMM  (PMU_BASE + 0x19C)         /* b31 HP triggers the LP core */
#define LP_TIMER        0x50112000u
#define LT_TAR1_LOW     (LP_TIMER + 0x08)
#define LT_TAR1_HIGH    (LP_TIMER + 0x0C)          /* b15:0 high word, b31 enable */
#define LT_UPDATE       (LP_TIMER + 0x10)
#define LT_BUF0_LOW     (LP_TIMER + 0x14)
#define LT_BUF0_HIGH    (LP_TIMER + 0x18)
#define LT_LP_INT_CLR   (LP_TIMER + 0x44)          /* b31 timer 1 */
#define WAKE_LP_TIMER_1 BIT(18)
#define WAKE_HP_CPU     BIT(22)
#define LP_RAM          0x50108000u

int ulp_load(const void *bin, size_t len)
{
    if (len > NDF_ULP_RESERVE) return -1;
    volatile uint32_t *d = (volatile uint32_t *)LP_RAM;
    const uint8_t *s = bin;
    for (size_t i = 0; i < len; i += 4) {
        uint32_t w = 0;
        for (size_t b = 0; b < 4 && i + b < len; b++) w |= (uint32_t)s[i + b] << (8 * b);
        d[i / 4] = w;
    }
    REG(LP_SYS_CORE_BOOT) = LP_RAM + 0x80;
    REG(LP_SYS_APP_BOOT) = LP_RAM + 0x80;
    return 0;
}

static void core_reset(void)
{
    REG_SET(LPPERI_RESET_EN, BIT(31)); REG_CLR(LPPERI_RESET_EN, BIT(31));
    REG_SET(LPPERI_CLK_EN, BIT(31));
    REG_SET(PMU_LP_PWR0, BIT(29) | BIT(30));            /* stall and reset the core while the LP system sleeps */
    REG_CLR(LPPERI_CPU, BIT(31));
}

static uint64_t rtc_ticks(void)
{
    REG(LT_UPDATE) = BIT(28);
    for (volatile int k = 0; k < 200; k++) {}
    return (uint64_t)(REG(LT_BUF0_HIGH) & 0xFFFF) << 32 | REG(LT_BUF0_LOW);
}

int ulp_run(uint32_t period_us)
{
    core_reset();
    volatile uint32_t *notes = (volatile uint32_t *)(LP_RAM + NDF_ULP_RESERVE);   /* [0] ticks low, [1] ticks high */
    if (period_us) {
        uint64_t ticks = (uint64_t)period_us * rtc_slow_hz() / 1000000u;
        notes[0] = (uint32_t)ticks; notes[1] = (uint32_t)(ticks >> 32);
        uint64_t t = rtc_ticks() + ticks;
        REG(LT_TAR1_LOW) = (uint32_t)t;
        REG(LT_LP_INT_CLR) = BIT(31);
        REG(LT_TAR1_HIGH) = ((uint32_t)(t >> 32) & 0xFFFF) | BIT(31);
        REG(PMU_LP_PWR2) = WAKE_LP_TIMER_1 | WAKE_HP_CPU;
    } else {
        REG(PMU_LP_PWR2) = WAKE_HP_CPU;
    }
    REG_SET(PMU_HP_LP_COMM, BIT(31));                   /* first run now */
    return 0;
}

void ulp_stop(void)
{
    REG_CLR(LT_TAR1_HIGH, BIT(31));
    REG(LT_LP_INT_CLR) = BIT(31);
    REG(PMU_LP_PWR2) = 0;
    REG_SET(LPPERI_RESET_EN, BIT(31)); REG_CLR(LPPERI_RESET_EN, BIT(31));
}

int ulp_fsm_run(uint32_t entry_word, uint32_t period_us) { (void)entry_word; (void)period_us; return -1; }   /* the P4 has no ULP-FSM */
