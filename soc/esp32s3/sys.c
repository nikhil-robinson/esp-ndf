/* ESP32-S3 bring-up: watchdogs, 240 MHz clock, core 1, delays, faults.
 * No ROM calls: everything is direct register access. */
#include "sys.h"
#include "soc.h"
#include "regi2c.h"
#include "xtensa.h"
#include "console.h"

extern uint32_t _bss_start[], _bss_end[];
extern void _start1(void);
int main(void);

static uint32_t cpu_hz = 20000000;            /* ROM leaves XTAL/2; fixed in clk_240mhz */
static void (*volatile cpu1_fn)(void);

uint32_t sys_cpu_hz(void) { return cpu_hz; }

void delay_us(uint32_t us)
{
    uint32_t t = ccount(), n = us * (cpu_hz / 1000000);
    while (ccount() - t < n) {}
}

uint32_t systimer_lo(void)
{
    REG(SYSTIMER_UNIT0_OP) = BIT(30);
    while (!(REG(SYSTIMER_UNIT0_OP) & BIT(29))) {}
    return REG(SYSTIMER_UNIT0_LO);
}

static void wdt_off(void)
{
    for (int n = 0; n < 2; n++) {
        REG(TIMG_WDTWPROTECT(n)) = WDT_WKEY; REG(TIMG_WDTCONFIG0(n)) = 0; REG(TIMG_WDTWPROTECT(n)) = 0;
    }
    REG(RTC_WDTWPROTECT) = WDT_WKEY; REG(RTC_WDTCONFIG0) = 0; REG(RTC_WDTWPROTECT) = 0;
    REG(RTC_SWD_WPROTECT) = SWD_WKEY; REG_SET(RTC_SWD_CONF, BIT(31)); REG(RTC_SWD_WPROTECT) = 0;
}

/* XTAL 40 MHz -> BBPLL 480 MHz -> CPU 240 MHz, APB stays 80 MHz. */
static void clk_240mhz(void)
{
    REG_FIELD(SYSTEM_SYSCLK_CONF, 10, 2, 0);              /* CPU on XTAL meanwhile */
    REG_CLR(RTC_OPTIONS0, BIT(6) | BIT(8) | BIT(10));    /* BBPLL + its i2c powered */
    REG_FIELD(SYSTEM_CPU_PER, 2, 1, 1);                  /* PLL 480 MHz */

    REG_CLR(I2C_ANA_CONF0, BIT(2)); REG_SET(I2C_ANA_CONF0, BIT(3));   /* cal start */
    regi2c_write(REGI2C_BBPLL, 4, 0x6B);                 /* MODE_HF                */
    regi2c_write(REGI2C_BBPLL, 2, 5 << 4 | 0);           /* DCHGP=5, REF_DIV=0     */
    regi2c_write(REGI2C_BBPLL, 3, 8);                    /* DIV                    */
    regi2c_write_mask(REGI2C_BBPLL, 5, 2, 0, 0);         /* DR1                    */
    regi2c_write_mask(REGI2C_BBPLL, 5, 6, 4, 0);         /* DR3                    */
    regi2c_write(REGI2C_BBPLL, 6, 1 << 6 | 3 << 4 | 3);  /* DLREF, DHREF, DCUR     */
    regi2c_write_mask(REGI2C_BBPLL, 9, 1, 0, 3);         /* VCO_DBIAS              */
    while (!(REG(I2C_ANA_CONF0) & BIT(24))) {}           /* cal done               */
    delay_us(10);
    REG_CLR(I2C_ANA_CONF0, BIT(3)); REG_SET(I2C_ANA_CONF0, BIT(2));   /* cal stop  */

    /* Core voltage up before 240 MHz. 28 is the untrimmed default.
     * ponytail: no eFuse PVT trim; add it with the eFuse driver. */
    regi2c_write_mask(REGI2C_DIG, 4, 4, 0, 28);          /* EXT_RTC_DREG */
    regi2c_write_mask(REGI2C_DIG, 6, 4, 0, 28);          /* EXT_DIG_DREG */
    delay_us(40);
    REG_FIELD(RTC_DATE, 13, 6, 0x7 >> 3);                /* all LDO slaves on */

    REG_FIELD(SYSTEM_CPU_PER, 0, 2, 2);                  /* CPUPERIOD 240 */
    REG_FIELD(SYSTEM_SYSCLK_CONF, 0, 10, 0);             /* PRE_DIV /1    */
    REG_FIELD(SYSTEM_SYSCLK_CONF, 10, 2, 1);             /* CPU on PLL    */
    cpu_hz = 240000000;
}

void cpu1_start(void (*fn)(void))
{
    cpu1_fn = fn;
    REG(SYSTEM_CORE1_CTRL1) = (uint32_t)_start1;         /* ROM core1 stub jumps here */
    REG_SET(SYSTEM_CORE1_CTRL0, BIT(1));                 /* clock on  */
    REG_CLR(SYSTEM_CORE1_CTRL0, BIT(0));                 /* un-stall  */
    REG_SET(SYSTEM_CORE1_CTRL0, BIT(2));                 /* reset pulse */
    REG_CLR(SYSTEM_CORE1_CTRL0, BIT(2));
}

void sys_start1(void)
{
    cpu1_fn();
    for (;;) {}
}

void sys_start0(void)
{
    for (uint32_t *p = _bss_start; p < _bss_end; p++) *p = 0;
    wdt_off();
    clk_240mhz();
    REG_SET(SYSTEM_PERIP_CLK0, PERIP0_SYSTIMER); REG_CLR(SYSTEM_PERIP_RST0, PERIP0_SYSTIMER);
    console_init();
    main();
    for (;;) {}
}

NORETURN void ndf_fault(uint32_t kind, uint32_t cause, uint32_t pc, uint32_t vaddr)
{
    static const char *k[] = { "?", "exception", "double exception", "unexpected vector" };
    printf("\n*** FAULT core%d: %s, EXCCAUSE=%lu PC=0x%08lx EXCVADDR=0x%08lx\n",
           core_id(), k[kind & 3], cause, pc, vaddr);
    for (;;) {}
}
