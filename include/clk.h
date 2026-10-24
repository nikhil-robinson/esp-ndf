/* Clock tree: CPU frequency, bus clocks, peripheral clock gates/resets. */
#pragma once
#include "ndf.h"

/* Peripheral ids: (register index << 8) | bit, see soc/<chip>/clk.c. */
typedef enum {
    PERIPH_UART0 = 0x002, PERIPH_I2S0 = 0x004, PERIPH_UART1 = 0x005, PERIPH_SPI2 = 0x006,
    PERIPH_I2C0 = 0x007, PERIPH_UHCI0 = 0x008, PERIPH_RMT = 0x009, PERIPH_PCNT = 0x00A,
    PERIPH_LEDC = 0x00B, PERIPH_TIMG0 = 0x00D, PERIPH_TIMG1 = 0x00F, PERIPH_SPI3 = 0x010,
    PERIPH_MCPWM0 = 0x011, PERIPH_I2C1 = 0x012, PERIPH_TWAI = 0x013, PERIPH_MCPWM1 = 0x014,
    PERIPH_I2S1 = 0x015, PERIPH_USB = 0x017, PERIPH_UART_MEM = 0x018, PERIPH_SARADC = 0x01C, PERIPH_SYSTIMER = 0x01D,
    PERIPH_GDMA = 0x106, PERIPH_SDMMC = 0x107, PERIPH_LCD_CAM = 0x108, PERIPH_UART2 = 0x109,
    PERIPH_AES = 0x101, PERIPH_SHA = 0x102, PERIPH_RSA = 0x103, PERIPH_DS = 0x104, PERIPH_HMAC = 0x105,
    PERIPH_DEDIC_GPIO = 0x207,
} periph_t;

void     periph_enable(periph_t p);      /* clock on, out of reset */
void     periph_disable(periph_t p);     /* into reset, clock off  */
void     periph_reset(periph_t p);       /* pulse reset, keep clock */

/* CPU: 240/160/80 (PLL) or 40 (XTAL). APB stays 80 MHz on PLL, = CPU on XTAL. */
int      clk_cpu_set_mhz(uint32_t mhz);
uint32_t clk_cpu_hz(void);
uint32_t clk_cpu_sync(void);             /* re-read the CPU clock from the hardware (after a boot loader set it) */
uint32_t clk_apb_hz(void);
/* Clock output on a pad: ch 1..3, src CLK_OUT_XTAL etc. (-1 off); pin must be that channel's pad (see clk.c). */
enum { CLK_OUT_PLL = 1, CLK_OUT_RC_SLOW = 4, CLK_OUT_XTAL = 5, CLK_OUT_PLL_80M = 13, CLK_OUT_RC_FAST = 14 };
int      clk_out(int ch, int src, int pin);
#define  CLK_XTAL_HZ  40000000u
