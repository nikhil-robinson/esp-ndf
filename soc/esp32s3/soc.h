/* ESP32-S3 register map (TRM v1.8). Only what drivers use; add as needed. */
#pragma once
#include "ndf.h"

#define SOC_CORES          2
#define SOC_XTAL_HZ        40000000u

/* ---- SYSTEM (TRM ch. 17) ------------------------------------------------ */
#define SYSTEM_BASE        0x600C0000
#define SYSTEM_CORE1_CTRL0 (SYSTEM_BASE + 0x000)   /* b0 RUNSTALL b1 CLKGATE_EN b2 RESETING */
#define SYSTEM_CORE1_CTRL1 (SYSTEM_BASE + 0x004)   /* core1 boot address, read by ROM        */
#define SYSTEM_CPU_PER     (SYSTEM_BASE + 0x010)   /* b1:0 CPUPERIOD_SEL  b2 PLL_FREQ_SEL     */
#define SYSTEM_PERIP_CLK0  (SYSTEM_BASE + 0x018)
#define SYSTEM_PERIP_RST0  (SYSTEM_BASE + 0x020)
#define SYSTEM_SYSCLK_CONF (SYSTEM_BASE + 0x060)   /* b9:0 PRE_DIV  b11:10 SOC_CLK_SEL        */
#define PERIP0_TIMG0       BIT(13)
#define PERIP0_TIMG1       BIT(15)
#define PERIP0_SYSTIMER    BIT(29)

/* ---- RTC_CNTL (TRM ch. 10) ---------------------------------------------- */
#define RTC_BASE           0x60008000
#define RTC_OPTIONS0       (RTC_BASE + 0x000)
#define RTC_WDTCONFIG0     (RTC_BASE + 0x098)
#define RTC_WDTWPROTECT    (RTC_BASE + 0x0B0)
#define RTC_SWD_CONF       (RTC_BASE + 0x0B4)
#define RTC_SWD_WPROTECT   (RTC_BASE + 0x0B8)
#define RTC_DATE           (RTC_BASE + 0x1FC)      /* b18:13 LDO SLAVE_PD                     */
#define WDT_WKEY           0x50D83AA1
#define SWD_WKEY           0x8F1D312A

/* ---- TIMG (TRM ch. 12) -------------------------------------------------- */
#define TIMG_BASE(n)       (0x6001F000 + (n) * 0x1000)
#define TIMG_T0CONFIG(n)   (TIMG_BASE(n) + 0x00)
#define TIMG_T0ALARMLO(n)  (TIMG_BASE(n) + 0x10)
#define TIMG_T0ALARMHI(n)  (TIMG_BASE(n) + 0x14)
#define TIMG_T0LOADLO(n)   (TIMG_BASE(n) + 0x18)
#define TIMG_T0LOADHI(n)   (TIMG_BASE(n) + 0x1C)
#define TIMG_T0LOAD(n)     (TIMG_BASE(n) + 0x20)
#define TIMG_WDTCONFIG0(n) (TIMG_BASE(n) + 0x48)
#define TIMG_WDTWPROTECT(n)(TIMG_BASE(n) + 0x64)
#define TIMG_INT_ENA(n)    (TIMG_BASE(n) + 0x70)
#define TIMG_INT_CLR(n)    (TIMG_BASE(n) + 0x7C)

/* ---- SYSTIMER (TRM ch. 11), 16 MHz from XTAL ---------------------------- */
#define SYSTIMER_BASE      0x60023000
#define SYSTIMER_UNIT0_OP  (SYSTIMER_BASE + 0x04)  /* b30 UPDATE  b29 VALUE_VALID             */
#define SYSTIMER_UNIT0_LO  (SYSTIMER_BASE + 0x44)

/* ---- GPIO / IO_MUX (TRM ch. 6) ------------------------------------------ */
#define GPIO_BASE          0x60004000
#define GPIO_OUT_W1TS      (GPIO_BASE + 0x008)
#define GPIO_OUT_W1TC      (GPIO_BASE + 0x00C)
#define GPIO_ENABLE_W1TS   (GPIO_BASE + 0x024)
#define GPIO_FUNC_OUT_SEL(n) (GPIO_BASE + 0x554 + (n) * 4)
#define IO_MUX_GPIO(n)     (0x60009004 + (n) * 4)

/* ---- Interrupt matrix (TRM ch. 9) --------------------------------------- */
#define INTMTX_MAP(core, src) (0x600C2000 + (core) * 0x800 + (src) * 4)
#define SRC_TG0_T0         50

/* ---- Analog register bus "regi2c" (undocumented; from ROM disassembly) -- */
#define I2C_ANA_CTRL(host) (0x6000E000 + (host) * 4) /* b25 BUSY b26 START b24 WRITE         */
#define I2C_ANA_CONF0      0x6000E040                /* BBPLL calibration stop/done bits     */
#define I2C_ANA_CONF1      0x6000E044                /* per-block masks (1 = masked)         */
#define I2C_ANA_CONF2      0x6000E048

/* ---- USB-Serial-JTAG (TRM ch. 33) --------------------------------------- */
#define USJ_EP1            0x60038000
#define USJ_EP1_CONF       0x60038004              /* b0 WR_DONE  b1 IN_EP_DATA_FREE         */
#define SYSTIMER_CONF        (SYSTIMER_BASE + 0x00)  /* b31 clk_en b30 unit0 work b24 target0 work */
#define SYSTIMER_TARGET0_HI  (SYSTIMER_BASE + 0x1C)
#define SYSTIMER_TARGET0_LO  (SYSTIMER_BASE + 0x20)
#define SYSTIMER_TARGET0_CONF (SYSTIMER_BASE + 0x34) /* b31 unit sel b30 period mode */
#define SYSTIMER_UNIT0_HI    (SYSTIMER_BASE + 0x40)
#define SYSTIMER_COMP0_LOAD  (SYSTIMER_BASE + 0x50)
#define SYSTIMER_INT_ENA     (SYSTIMER_BASE + 0x64)
#define SYSTIMER_INT_CLR     (SYSTIMER_BASE + 0x6C)
#define SYSTIMER_INT_ST      (SYSTIMER_BASE + 0x70)
#define SRC_SYSTIMER_TARGET0 57
