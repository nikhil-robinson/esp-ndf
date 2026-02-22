/* =============================================================================
 * rtc_cntl_reg.h  –  ESP32-S3 RTC Controller registers
 *
 * The RTC Controller governs:
 *   - Power management (sleep modes, wakeup sources)
 *   - Brown-out detection (BOD)
 *   - RTC watchdog timer
 *   - Crystal oscillator configuration
 *   - CPU sleep/wake-up control
 *   - Deep-sleep / light-sleep modes
 *
 * Base: 0x60008000
 * Source: ESP32-S3 TRM v1.4, Chapter 8 (Low-Power Management)
 * ============================================================================= */
#ifndef ESP32S3_RTC_CNTL_REG_H
#define ESP32S3_RTC_CNTL_REG_H

#include "soc/soc.h"

/* ---- Register offsets --------------------------------------------------- */
#define RTC_CNTL_OPTIONS0_REG           (DR_REG_RTCCNTL_BASE + 0x00)
#define RTC_CNTL_SLP_TIMER0_REG         (DR_REG_RTCCNTL_BASE + 0x04)
#define RTC_CNTL_SLP_TIMER1_REG         (DR_REG_RTCCNTL_BASE + 0x08)
#define RTC_CNTL_TIME_UPDATE_REG        (DR_REG_RTCCNTL_BASE + 0x0C)
#define RTC_CNTL_TIME_LOW0_REG          (DR_REG_RTCCNTL_BASE + 0x10)
#define RTC_CNTL_TIME_HIGH0_REG         (DR_REG_RTCCNTL_BASE + 0x14)
#define RTC_CNTL_STATE0_REG             (DR_REG_RTCCNTL_BASE + 0x18)
#define RTC_CNTL_TIMER1_REG             (DR_REG_RTCCNTL_BASE + 0x1C)
#define RTC_CNTL_TIMER2_REG             (DR_REG_RTCCNTL_BASE + 0x20)
#define RTC_CNTL_TIMER3_REG             (DR_REG_RTCCNTL_BASE + 0x24)
#define RTC_CNTL_TIMER4_REG             (DR_REG_RTCCNTL_BASE + 0x28)
#define RTC_CNTL_TIMER5_REG             (DR_REG_RTCCNTL_BASE + 0x2C)
#define RTC_CNTL_TIMER6_REG             (DR_REG_RTCCNTL_BASE + 0x30)

/* Analog configuration */
#define RTC_CNTL_ANA_CONF_REG           (DR_REG_RTCCNTL_BASE + 0x34)

/* Reset reasons */
#define RTC_CNTL_RESET_STATE_REG        (DR_REG_RTCCNTL_BASE + 0x38)

/* Wakeup sources */
#define RTC_CNTL_WAKEUP_STATE_REG       (DR_REG_RTCCNTL_BASE + 0x3C)
#define RTC_CNTL_INT_ENA_RTC_REG        (DR_REG_RTCCNTL_BASE + 0x40)
#define RTC_CNTL_INT_RAW_RTC_REG        (DR_REG_RTCCNTL_BASE + 0x44)
#define RTC_CNTL_INT_ST_RTC_REG         (DR_REG_RTCCNTL_BASE + 0x48)
#define RTC_CNTL_INT_CLR_RTC_REG        (DR_REG_RTCCNTL_BASE + 0x4C)

/* RTC store registers (survive deep sleep) */
#define RTC_CNTL_STORE0_REG             (DR_REG_RTCCNTL_BASE + 0x50)
#define RTC_CNTL_STORE1_REG             (DR_REG_RTCCNTL_BASE + 0x54)
#define RTC_CNTL_STORE2_REG             (DR_REG_RTCCNTL_BASE + 0x58)
#define RTC_CNTL_STORE3_REG             (DR_REG_RTCCNTL_BASE + 0x5C)
#define RTC_CNTL_EXT_XTL_CONF_REG      (DR_REG_RTCCNTL_BASE + 0x60)

/* Brown-out detector */
#define RTC_CNTL_BROWN_OUT_REG          (DR_REG_RTCCNTL_BASE + 0xD4)
#define RTC_CNTL_BROWN_OUT_ENA_S        31
#define RTC_CNTL_BROWN_OUT_ENA_M        (1u << 31)
#define RTC_CNTL_BROWN_OUT_RST_WAIT_S   4
#define RTC_CNTL_BROWN_OUT_RST_ENA_S    30

/* RTC WDT (watchdog) */
#define RTC_CNTL_WDTCONFIG0_REG         (DR_REG_RTCCNTL_BASE + 0x90)
#define RTC_CNTL_WDTCONFIG1_REG         (DR_REG_RTCCNTL_BASE + 0x94)
#define RTC_CNTL_WDTCONFIG2_REG         (DR_REG_RTCCNTL_BASE + 0x98)
#define RTC_CNTL_WDTCONFIG3_REG         (DR_REG_RTCCNTL_BASE + 0x9C)
#define RTC_CNTL_WDTCONFIG4_REG         (DR_REG_RTCCNTL_BASE + 0xA0)
#define RTC_CNTL_WDTFEED_REG            (DR_REG_RTCCNTL_BASE + 0xA4)
#define RTC_CNTL_WDTWPROTECT_REG        (DR_REG_RTCCNTL_BASE + 0xA8)
#define RTC_WDT_WKEY                    0x50D83AA1UL   /* WDT write-protect key */

/* ---- RTC_CNTL_OPTIONS0_REG fields --------------------------------------- */
#define RTC_CNTL_SW_SYS_RST_S           31  /* [31]   Software system reset  */
#define RTC_CNTL_DG_WRAP_FORCE_NORST_S  30
#define RTC_CNTL_DG_WRAP_FORCE_RST_S    29
#define RTC_CNTL_ANALOG_FORCE_NORST_S   28
#define RTC_CNTL_ANALOG_FORCE_RST_S     27
#define RTC_CNTL_SW_PROCPU_RST_S        12  /* [12]   CPU0 software reset    */
#define RTC_CNTL_SW_APPCPU_RST_S        11  /* [11]   CPU1 software reset    */
#define RTC_CNTL_BB_I2C_FORCE_PD_S      6
#define RTC_CNTL_BB_I2C_FORCE_PU_S      5
#define RTC_CNTL_BBPLL_FORCE_PD_S       4
#define RTC_CNTL_BBPLL_FORCE_PU_S       3
#define RTC_CNTL_BBPLL_I2C_FORCE_PD_S   2
#define RTC_CNTL_BBPLL_I2C_FORCE_PU_S   1
#define RTC_CNTL_BSK_DSIG_S             0

/* ---- RTC_CNTL_STATE0_REG fields ----------------------------------------- */
#define RTC_CNTL_SLEEP_EN_S             0   /* Enter sleep mode              */
#define RTC_CNTL_SLP_WAKEUP_S          16   /* Wakeup condition met          */

/* ---- Brown-out helper --------------------------------------------------- */
/** Disable brown-out detector (call early in board_init if BOD causes spurious
 *  resets during clock ramp-up – re-enable once PLL is stable). */
#define RTC_BOD_DISABLE() \
    REG_CLR_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA_M)
#define RTC_BOD_ENABLE() \
    REG_SET_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA_M)

/* ---- RTC WDT helpers ---------------------------------------------------- */
/** Disable the RTC WDT.  Must unlock write-protect first. */
#define RTC_WDT_DISABLE() do {                                          \
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, RTC_WDT_WKEY);                 \
    REG_WRITE(RTC_CNTL_WDTCONFIG0_REG, 0);                             \
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0);                            \
} while(0)

#endif /* ESP32S3_RTC_CNTL_REG_H */
