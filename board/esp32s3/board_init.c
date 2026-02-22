/* =============================================================================
 * board_init.c  –  ESP32-S3 hardware initialisation
 *
 * This module performs the minimum hardware setup required before any
 * application code runs.  It is called from __start_c() in start.S.
 *
 * Clock topology (ESP32-S3):
 *
 *   XTAL (40 MHz) ──→ BBPLL (480 MHz) ──→ CPU divider ──→ CPU_CLK
 *                                       ──→ APB divider ──→ APB_CLK (80 MHz)
 *                  ──→ RC_FAST (17.5 MHz)
 *                  ──→ RC_SLOW  (150 kHz RTC)
 *
 * Power-on defaults (set by ROM bootloader):
 *   - CPU runs at 80 MHz (BBPLL at 320 MHz, /4)
 *   - APB runs at 80 MHz
 *
 * This module switches CPU to 240 MHz by:
 *   1. Switching to 480 MHz BBPLL (CPU_PER_CONF.PLL_FREQ_SEL = 1)
 *   2. Setting CPU divider to /2 (CPUPERIOD_SEL = 2 → 240 MHz)
 * ============================================================================= */
#include "board.h"
#include "drivers/uart.h"
#include "drivers/gpio.h"

/* ---- IO MUX pad table --------------------------------------------------- */
/*
 * io_mux_gpio_reg[n] gives the IO MUX register address for GPIO n.
 * Gaps (GPIO22-25 don't exist on ESP32-S3) are filled with 0.
 */
const uint32_t io_mux_gpio_reg[49] = {
    [0]  = IO_MUX_GPIO0_REG,  [1]  = IO_MUX_GPIO1_REG,
    [2]  = IO_MUX_GPIO2_REG,  [3]  = IO_MUX_GPIO3_REG,
    [4]  = IO_MUX_GPIO4_REG,  [5]  = IO_MUX_GPIO5_REG,
    [6]  = IO_MUX_GPIO6_REG,  [7]  = IO_MUX_GPIO7_REG,
    [8]  = IO_MUX_GPIO8_REG,  [9]  = IO_MUX_GPIO9_REG,
    [10] = IO_MUX_GPIO10_REG, [11] = IO_MUX_GPIO11_REG,
    [12] = IO_MUX_GPIO12_REG, [13] = IO_MUX_GPIO13_REG,
    [14] = IO_MUX_GPIO14_REG, [15] = IO_MUX_GPIO15_REG,
    [16] = IO_MUX_GPIO16_REG, [17] = IO_MUX_GPIO17_REG,
    [18] = IO_MUX_GPIO18_REG, [19] = IO_MUX_GPIO19_REG,
    [20] = IO_MUX_GPIO20_REG, [21] = IO_MUX_GPIO21_REG,
    [22] = 0,  [23] = 0,  [24] = 0,  [25] = 0,  /* These GPIOs don't exist */
    [26] = IO_MUX_GPIO26_REG, [27] = IO_MUX_GPIO27_REG,
    [28] = IO_MUX_GPIO28_REG, [29] = IO_MUX_GPIO29_REG,
    [30] = IO_MUX_GPIO30_REG, [31] = IO_MUX_GPIO31_REG,
    [32] = IO_MUX_GPIO32_REG, [33] = IO_MUX_GPIO33_REG,
    [34] = IO_MUX_GPIO34_REG, [35] = IO_MUX_GPIO35_REG,
    [36] = IO_MUX_GPIO36_REG, [37] = IO_MUX_GPIO37_REG,
    [38] = IO_MUX_GPIO38_REG, [39] = IO_MUX_GPIO39_REG,
    [40] = IO_MUX_GPIO40_REG, [41] = IO_MUX_GPIO41_REG,
    [42] = IO_MUX_GPIO42_REG, [43] = IO_MUX_GPIO43_REG,
    [44] = IO_MUX_GPIO44_REG, [45] = IO_MUX_GPIO45_REG,
    [46] = IO_MUX_GPIO46_REG, [47] = IO_MUX_GPIO47_REG,
    [48] = IO_MUX_GPIO48_REG,
};

/* ---- UART base address table (declared extern in uart_reg.h) ------------ */
const uint32_t uart_base[3] = {
    DR_REG_UART0_BASE,
    DR_REG_UART1_BASE,
    DR_REG_UART2_BASE,
};

/* ============================================================================
 * Private helpers
 * ============================================================================ */

/**
 * @brief  Disable all hardware watchdog timers inherited from the ROM bootloader.
 *
 * The ROM bootloader enables TIMG0, TIMG1, and RTC watchdogs with timeouts of
 * ~8 seconds.  We must disable them before our slow initialisation sequences.
 */
static void disable_wdts(void)
{
    /* ---- Timer Group 0 WDT ---- */
    REG_WRITE(TIMG0_BASE + 0xA4, TIMG0_WDT_KEY);  /* unlock write protect    */
    REG_WRITE(TIMG0_BASE + 0x8C, 0);               /* disable WDT             */
    REG_WRITE(TIMG0_BASE + 0xA4, 0);               /* re-lock                 */

    /* ---- Timer Group 1 WDT ---- */
    REG_WRITE(TIMG1_BASE + 0xA4, TIMG0_WDT_KEY);
    REG_WRITE(TIMG1_BASE + 0x8C, 0);
    REG_WRITE(TIMG1_BASE + 0xA4, 0);

    /* ---- RTC WDT ---- */
    RTC_WDT_DISABLE();
}

/* Timer group bases (identical to defines in start.S – avoid redefinition via macro) */
#define TIMG0_BASE      0x6001F000UL
#define TIMG1_BASE      0x60020000UL
#define TIMG0_WDT_KEY   0x50D83AA1UL

/**
 * @brief  Configure BBPLL for 480 MHz and switch CPU to 240 MHz.
 *
 * Background:
 *   On power-on / ROM exit the CPU is clocked at 80 MHz (BBPLL at 320 MHz).
 *   The BBPLL frequency is controlled by RTC_CNTL analog I2C registers
 *   (undocumented I²C bus inside the PMU block).  Switching to 480 MHz is
 *   done by setting SYSTEM_CPU_PER_CONF.PLL_FREQ_SEL = 1.
 *
 *   Then selecting 240 MHz requires CPUPERIOD_SEL = 2.
 *
 * NOTE: Between the PLL frequency select and the CPU divider change there
 *       should be a short delay (~50 µs) for the PLL to lock.  We implement
 *       this as a simple loop because the CCOUNT timer is already running.
 */
static void switch_cpu_to_240mhz(void)
{
    uint32_t cfg;

    /* Temporarily disable BOD during PLL frequency change to avoid false resets */
    RTC_BOD_DISABLE();

    /* Step 1: Switch BBPLL to 480 MHz
     *   CPU_PER_CONF.PLL_FREQ_SEL: 0 = 320 MHz  1 = 480 MHz
     */
    cfg = REG_READ(SYSTEM_CPU_PER_CONF_REG);
    cfg |= (1u << SYSTEM_PLL_FREQ_SEL_S);   /* select 480 MHz PLL */
    REG_WRITE(SYSTEM_CPU_PER_CONF_REG, cfg);

    /* Wait ~50 µs for PLL to lock.
     * At the current ~80 MHz, 50 µs ≈ 4000 cycles.
     */
    {
        volatile uint32_t delay = 4000;
        while (delay--) { __asm__ volatile ("nop"); }
    }

    /* Step 2: Set CPU divider to ÷2 (CPU_CLK = 480/2 = 240 MHz)
     *   CPUPERIOD_SEL: 0=80MHz, 1=160MHz, 2=240MHz
     */
    cfg = REG_READ(SYSTEM_CPU_PER_CONF_REG);
    cfg &= ~SYSTEM_CPUPERIOD_SEL_M;
    cfg |= (SYSTEM_CPUPERIOD_240MHZ << SYSTEM_CPUPERIOD_SEL_S);
    REG_WRITE(SYSTEM_CPU_PER_CONF_REG, cfg);

    /* Step 3: Update clock enable in CPU_PER_CONF (not strictly necessary but safe) */
    cfg |= (1u << SYSTEM_CLK_EN_S);
    REG_WRITE(SYSTEM_CPU_PER_CONF_REG, cfg);

    /* APB remains at 80 MHz (AHB bus frequency is independent of CPU PLL select) */

    /* Re-enable BOD now that PLL is stable */
    RTC_BOD_ENABLE();
}

/**
 * @brief  Enable peripheral clocks for GPIO, UART, SYSTIMER.
 * Other peripherals must enable their own clocks in their init functions.
 */
static void enable_peripheral_clocks(void)
{
    uint32_t en0 = REG_READ(SYSTEM_PERIP_CLK_EN0_REG);

    /* Keep existing clocks enabled, add our required set */
    en0 |= SYSTEM_UART_CLK_EN    |   /* UART0                */
           SYSTEM_UART1_CLK_EN   |   /* UART1                */
           SYSTEM_TIMERS_CLK_EN  |   /* Timer groups         */
           SYSTEM_SYSTIMER_CLK_EN;   /* SYSTIMER (64-bit)    */
    REG_WRITE(SYSTEM_PERIP_CLK_EN0_REG, en0);

    /* Release peripherals from reset */
    uint32_t rst0 = REG_READ(SYSTEM_PERIP_RST_EN0_REG);
    rst0 &= ~(SYSTEM_UART_CLK_EN    |
              SYSTEM_UART1_CLK_EN   |
              SYSTEM_TIMERS_CLK_EN  |
              SYSTEM_SYSTIMER_CLK_EN);
    REG_WRITE(SYSTEM_PERIP_RST_EN0_REG, rst0);

    /* GPIO clock is always running (it is part of the always-on power domain) */
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void board_init(void)
{
    /* 1. Kill all watchdogs immediately */
    disable_wdts();

    /* 2. Ramp CPU to 240 MHz */
    switch_cpu_to_240mhz();

    /* 3. Enable peripheral bus clocks */
    enable_peripheral_clocks();

    /* 4. Initialise interrupt controller */
    intr_init();

    /* 5. Initialise debug UART (UART0) */
    {
        uart_config_t uart_cfg = {
            .baud_rate  = BOARD_DEBUG_UART_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .stop_bits  = UART_STOP_BITS_1,
            .parity     = UART_NO_PARITY,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        };
        uart_init(BOARD_DEBUG_UART_PORT, &uart_cfg);
    }

    /* 6. Enable interrupts globally (INTLEVEL → 0) */
    xtensa_intr_enable();
}


void board_putc(char c)
{
    uart_putc(BOARD_DEBUG_UART_PORT, (uint8_t)c);
}


void board_puts(const char *s)
{
    while (*s) {
        board_putc(*s++);
    }
}


void board_delay_us(uint32_t us)
{
    /* CCOUNT increments at CPU clock rate (240 MHz after board_init) */
    uint32_t cycles = us * (BOARD_CPU_FREQ_HZ / 1000000UL);
    uint32_t start  = xtensa_ccount();
    while ((xtensa_ccount() - start) < cycles) {
        /* spin */
    }
}


void board_delay_ms(uint32_t ms)
{
    board_delay_us(ms * 1000UL);
}
