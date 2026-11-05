/* ESP32-P4 (v3.x) register map. Values from ESP-IDF v6.1 soc/esp32p4/register/hw_ver3 and include/soc/interrupts.h.
 * Only what the chip layer and the P4 drivers use; add as needed. Names follow the S3 soc.h where the block is the same. */
#pragma once
#include "ndf.h"

#define SOC_CORES          2
#define SOC_XTAL_HZ        40000000u

#define HPPERIPH0          0x50000000u
#define HPPERIPH1          0x500C0000u
#define LPAON              0x50110000u
#define LPPERIPH           0x50120000u

/* ---- HP system clock / reset (HP_SYS_CLKRST) --------------------------- */
#define HP_CLKRST_BASE        (HPPERIPH1 + 0x26000)
#define HP_CLKRST_SOC_CLK_CTRL0 (HP_CLKRST_BASE + 0x14)   /* b4 CORE1_CPU_CLK_EN */
#define HP_CLKRST_HP_RST_EN0  (HP_CLKRST_BASE + 0xC0)     /* b8 RST_EN_CORE1_GLOBAL */
#define HP_SYS_BASE           (HPPERIPH1 + 0x25000)
#define HP_SYS_CORESTALLED_ST (HP_SYS_BASE + 0x64)        /* b0 core0, b1 core1 stalled */
#define HP_SYS_CPU_INT_FROM_CPU(n) (HP_SYS_BASE + 0x10 + 4 * (n))    /* cross-core software interrupts 0..3 */

/* ---- LP always-on ------------------------------------------------------- */
#define FLASH_SPI1_BASE    (HPPERIPH0 + 0x8D000)       /* SPI1: flash commands (SPI0 is the cache path) */
#define LP_SYS_BASE           (LPAON + 0x0000)
#define LP_SYS_BOOT_ADDR_HP_CORE1 (LP_SYS_BASE + 0x164)   /* core 1 reset vector (ROM ets_set_appcpu_boot_addr) */
#define LP_CLKRST_BASE        (LPAON + 0x1000)
#define LP_CLKRST_RESET_CAUSE (LP_CLKRST_BASE + 0x10)     /* b12:7 HP core0 cause, b19:14 HP core1 cause */
#define PMU_BASE              (LPAON + 0x5000)
#define PMU_CPU_SW_STALL      (PMU_BASE + 0x200)          /* b31:24 core0, b23:16 core1 stall code: 0x86 stall, 0xFF run */
#define LP_WDT_BASE           (LPAON + 0x6000)
#define LP_WDT_CONFIG0        (LP_WDT_BASE + 0x00)
#define LP_WDT_FEED           (LP_WDT_BASE + 0x14)
#define LP_WDT_WPROTECT       (LP_WDT_BASE + 0x18)
#define LP_WDT_SWD_CONFIG     (LP_WDT_BASE + 0x1C)        /* b30 SWD_DISABLE, b18 SWD_AUTO_FEED_EN */
#define LP_WDT_SWD_WPROTECT   (LP_WDT_BASE + 0x20)
#define WDT_WKEY              0x50D83AA1u                 /* TIMG, LP WDT and super WDT (IDF lpwdt_ll.h) */
#define SWD_WKEY              0x50D83AA1u

/* ---- TIMG (same IP as the S3) ------------------------------------------- */
#define TIMG_BASE(n)       (HPPERIPH1 + 0x2000 + (n) * 0x1000)
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

/* ---- SYSTIMER (same IP and offsets as the S3), 16 MHz ------------------- */
#define SYSTIMER_BASE        (HPPERIPH1 + 0x22000)
#define SYSTIMER_CONF        (SYSTIMER_BASE + 0x00)
#define SYSTIMER_UNIT0_OP    (SYSTIMER_BASE + 0x04)
#define SYSTIMER_TARGET0_HI  (SYSTIMER_BASE + 0x1C)
#define SYSTIMER_TARGET0_LO  (SYSTIMER_BASE + 0x20)
#define SYSTIMER_TARGET0_CONF (SYSTIMER_BASE + 0x34)
#define SYSTIMER_TARGET1_CONF (SYSTIMER_BASE + 0x38)
#define SYSTIMER_UNIT0_HI    (SYSTIMER_BASE + 0x40)
#define SYSTIMER_UNIT0_LO    (SYSTIMER_BASE + 0x44)
#define SYSTIMER_COMP0_LOAD  (SYSTIMER_BASE + 0x50)
#define SYSTIMER_COMP1_LOAD  (SYSTIMER_BASE + 0x54)
#define SYSTIMER_INT_ENA     (SYSTIMER_BASE + 0x64)
#define SYSTIMER_INT_CLR     (SYSTIMER_BASE + 0x6C)
#define SYSTIMER_INT_ST      (SYSTIMER_BASE + 0x70)

/* ---- UART (same IP as the S3) and USB-Serial-JTAG ------------------------ */
#define UART_BASE(n)       (HPPERIPH1 + 0xA000 + (n) * 0x1000)   /* UART0..4 */
#define UART_FIFO(n)       (UART_BASE(n) + 0x00)
#define UART_STATUS(n)     (UART_BASE(n) + 0x1C)                 /* b23:16 TXFIFO_CNT, b7:0 RXFIFO_CNT */
#define USJ_BASE           (HPPERIPH1 + 0x12000)
#define USJ_EP1            (USJ_BASE + 0x00)
#define USJ_EP1_CONF       (USJ_BASE + 0x04)                     /* b0 WR_DONE  b1 IN_EP_DATA_FREE  b2 OUT_EP_DATA_AVAIL */

/* ---- GPIO / IO_MUX ------------------------------------------------------- */
#define GPIO_BASE          (HPPERIPH1 + 0x20000)
#define IO_MUX_BASE        (HPPERIPH1 + 0x21000)

/* ---- interrupt matrix: one 32-bit map register per source per core; value = CLIC id (16 + line) ---------- */
#define INTMTX_MAP(core, src) (HPPERIPH1 + 0x16000 + (core) * 0x800 + (src) * 4)
#define SRC_USB_SERIAL_JTAG  22
#define SRC_UART0            31
#define SRC_UART1            32
#define SRC_TG0_T0           46
#define SRC_TG0_T1           47
#define SRC_TG0_WDT          48
#define SRC_TG1_T0           49
#define SRC_TG1_T1           50
#define SRC_TG1_WDT          51
#define SRC_SYSTIMER_TARGET0 53
#define SRC_SYSTIMER_TARGET1 54
#define SRC_SYSTIMER_TARGET2 55
#define SRC_GPIO             74
#define SRC_FROM_CPU0        79
#define SRC_MAX              136

/* more interrupt matrix sources (ESP-IDF v6.1 soc/esp32p4/include/soc/interrupts.h) */
#define SRC_LP_ADC           9
#define SRC_TSENS            15
#define SRC_SPI2             25
#define SRC_SPI3             26
#define SRC_I2S0             27
#define SRC_I2S1             28
#define SRC_I2S2             29
#define SRC_UHCI0            30
#define SRC_LCD_CAM          36
#define SRC_ADC              37
#define SRC_TWAI0            40
#define SRC_TWAI1            41
#define SRC_TWAI2            42
#define SRC_RMT              43
#define SRC_I2C0             44
#define SRC_I2C1             45
#define SRC_LEDC             52
#define SRC_AHB_PDMA_IN(c)   (56 + (c))
#define SRC_AHB_PDMA_OUT(c)  (59 + (c))
#define SRC_AXI_PDMA_IN(c)   (62 + (c))
#define SRC_AXI_PDMA_OUT(c)  (65 + (c))
#define SRC_USB_OTG          93
#define SRC_PCNT             111

/* DMA controllers */
#define AHB_DMA_BASE         (HPPERIPH0 + 0x85000)
#define AXI_DMA_BASE         (HPPERIPH0 + 0x8A000)
#define CACHE_CTRL_BASE      0x3FF10000u

/* ---- USB 2.0 OTG high speed on the internal UTMI PHY (ESP-IDF v6.1 usb_utmi_ll.h, tinyusb dwc2_esp32.h) ---- */
#define USB_OTG_BASE          HPPERIPH0                 /* DWC2 HS core; the FS core is at +0x40000 */
#define USB_OTG_EPS           16
#define USB_OTG_FIFO_WORDS    1024
#define LP_AON_CLKRST_USB     (LP_CLKRST_BASE + 0x48)   /* b30 OTG20 PHY reference clock, b2 reset OTG20, b1 reset PHY */
#define HP_SYSTEM_USBOTG20_CTRL (HP_SYS_BASE + 0x15C)   /* b21 SUSPENDM (software must set it, else detach is not seen) */
#define USB_UTMI_FC_06        (HPPERIPH0 + 0x9C000 + 0x18)   /* b0 LS parallel enable, b3 LS keep-alive */
#define LP_SYS_USB_PHY_CTRL   (LPAON + 0x204)           /* b1 DP pulldown, b2 DM pulldown (host only) */
