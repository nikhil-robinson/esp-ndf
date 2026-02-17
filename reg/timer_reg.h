/*
 * ESP32-S3 Timer Register Definitions
 */

#ifndef REG_TIMER_REG_H
#define REG_TIMER_REG_H

#include "../arch/esp32s3/esp32s3.h"

/* Timer Group 0 base */
#define TIMG_T0CONFIG_REG(g)        (DR_REG_TIMG0_BASE + (g) * 0x1000 + 0x0000)
#define TIMG_T0LO_REG(g)            (DR_REG_TIMG0_BASE + (g) * 0x1000 + 0x0004)
#define TIMG_T0HI_REG(g)            (DR_REG_TIMG0_BASE + (g) * 0x1000 + 0x0008)
#define TIMG_T0UPDATE_REG(g)        (DR_REG_TIMG0_BASE + (g) * 0x1000 + 0x000C)
#define TIMG_T0ALARMLO_REG(g)       (DR_REG_TIMG0_BASE + (g) * 0x1000 + 0x0010)
#define TIMG_T0ALARMHI_REG(g)       (DR_REG_TIMG0_BASE + (g) * 0x1000 + 0x0014)
#define TIMG_T0LOADLO_REG(g)        (DR_REG_TIMG0_BASE + (g) * 0x1000 + 0x0018)
#define TIMG_T0LOADHI_REG(g)        (DR_REG_TIMG0_BASE + (g) * 0x1000 + 0x001C)
#define TIMG_T0LOAD_REG(g)          (DR_REG_TIMG0_BASE + (g) * 0x1000 + 0x0020)

/* Timer config bits */
#define TIMG_T0_ALARM_EN            (1 << 10)
#define TIMG_T0_LEVEL_INT_EN        (1 << 11)
#define TIMG_T0_EDGE_INT_EN         (1 << 12)
#define TIMG_T0_DIVIDER_S           13
#define TIMG_T0_DIVIDER_M           (0xFFFF << TIMG_T0_DIVIDER_S)
#define TIMG_T0_AUTORELOAD          (1 << 29)
#define TIMG_T0_INCREASE            (1 << 30)
#define TIMG_T0_EN                  (1 << 31)

/* Watchdog registers */
#define TIMG_WDTCONFIG0_REG(g)      (DR_REG_TIMG0_BASE + (g) * 0x1000 + 0x0048)
#define TIMG_WDTWPROTECT_REG(g)     (DR_REG_TIMG0_BASE + (g) * 0x1000 + 0x0064)

#define TIMG_WDT_PROTECT_KEY        0x50D83AA1

#endif /* REG_TIMER_REG_H */
