/* =============================================================================
 * intr.h  –  Xtensa LX7 interrupt controller API for ESP32-S3
 *
 * ESP32-S3 has 32 interrupt sources per CPU and 6 priority levels (1-6)
 * plus NMI (level 7).  This module manages:
 *   - Mapping peripheral interrupt sources to CPU interrupt numbers
 *   - Registering ISR function pointers
 *   - Enabling/disabling individual interrupt channels
 *
 * Interrupt delivery path:
 *   Peripheral → Interrupt Matrix (INTMTX) → CPU interrupt input (0..31)
 *   → Xtensa INTENABLE/INTSET → Level-N vector → ISR dispatcher
 * ============================================================================= */
#ifndef NDF_INTR_H
#define NDF_INTR_H

#include "types.h"
#include "xtensa_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CPU interrupt numbers (Xtensa INTENABLE bits) ---------------------- */
#define CPU_INTR_NUM_MAX   32

/* ---- Interrupt priority levels (Xtensa PS.INTLEVEL) -------------------- */
#define INTR_LEVEL_1    1   /* most peripheral IRQs */
#define INTR_LEVEL_2    2
#define INTR_LEVEL_3    3   /* e.g. timer LEVEL3 tick */
#define INTR_LEVEL_4    4
#define INTR_LEVEL_5    5   /* e.g. performance monitor */
#define INTR_LEVEL_NMI  7   /* watchdog NMI – cannot be masked */

/* ---- ISR callback type -------------------------------------------------- */
typedef void (*intr_isr_t)(void *arg);

/* ---- Interrupt descriptor ----------------------------------------------- */
typedef struct {
    intr_isr_t  handler;    /* ISR function pointer (NULL = unhandled) */
    void       *arg;        /* opaque argument passed to handler        */
} intr_desc_t;

/* ---- Interrupt-matrix peripheral source IDs (ESP32-S3 TRM Table 9-6) --- */
typedef enum {
    INTR_SRC_WIFI_MAC      = 0,
    INTR_SRC_WIFI_NMI      = 1,
    INTR_SRC_UART0         = 21,
    INTR_SRC_UART1         = 22,
    INTR_SRC_UART2         = 23,
    INTR_SRC_SPI0          = 27,
    INTR_SRC_SPI1          = 28,
    INTR_SRC_SPI2          = 29,
    INTR_SRC_SPI3          = 30,
    INTR_SRC_I2S0          = 35,
    INTR_SRC_I2S1          = 36,
    INTR_SRC_I2C0          = 50,
    INTR_SRC_GPIO_INTR     = 55,    /* GPIO interrupt                        */
    INTR_SRC_GPIO_NMI      = 56,    /* GPIO NMI interrupt                    */
    INTR_SRC_TIMER0        = 73,    /* Timer group 0 timer 0                 */
    INTR_SRC_TIMER1        = 74,    /* Timer group 0 timer 1                 */
    INTR_SRC_TIMER2        = 75,    /* Timer group 1 timer 0                 */
    INTR_SRC_TIMER3        = 76,    /* Timer group 1 timer 1                 */
    INTR_SRC_WDT0          = 77,    /* Timer group 0 watchdog                */
    INTR_SRC_WDT1          = 78,    /* Timer group 1 watchdog                */
    INTR_SRC_SPI_FLASH     = 84,
    INTR_SRC_MAX           = 99,
} intr_src_t;

/* ---- Interrupt matrix register (INTMTX) base address -------------------- */
#define INTERRUPT_MATRIX_BASE  0x600C2000UL

/* PRO CPU (core 0): source-to-CPU-interrupt mapping registers
 * Register at (INTERRUPT_MATRIX_BASE + src * 4) holds the CPU interrupt
 * number (0..31) mapped to peripheral source 'src'.
 */
#define INTMTX_PRO_MAP_REG(src)  \
    (*(volatile uint32_t *)(INTERRUPT_MATRIX_BASE + (src) * 4))

/* ---- Public API --------------------------------------------------------- */

/**
 * @brief  Initialise the interrupt subsystem.
 * Clears all CPU interrupt enables and zeroes the ISR table.
 */
void intr_init(void);

/**
 * @brief  Allocate and configure a CPU interrupt for a peripheral source.
 *
 * @param  src        Peripheral interrupt source (see intr_src_t).
 * @param  level      Desired Xtensa interrupt priority level (1..5).
 * @param  handler    ISR function pointer.
 * @param  arg        Opaque argument passed to handler.
 * @param  cpu_intr   [out] Assigned CPU interrupt number (0..31).
 * @return 0 on success, -1 if no free CPU interrupt slot at the given level.
 */
int  intr_alloc(intr_src_t src, int level, intr_isr_t handler,
                void *arg, int *cpu_intr);

/**
 * @brief  Enable a previously allocated CPU interrupt number.
 * @param  cpu_intr  CPU interrupt number returned by intr_alloc().
 */
void intr_enable(int cpu_intr);

/**
 * @brief  Disable a CPU interrupt number.
 */
void intr_disable(int cpu_intr);

/**
 * @brief  Free a CPU interrupt allocation and unmap the peripheral source.
 */
void intr_free(int cpu_intr);

/* ---- Global disable/enable (maps to xtensa_intr_disable/enable) --------- */
#define INTR_GLOBAL_DISABLE()  xtensa_intr_disable()
#define INTR_GLOBAL_ENABLE()   xtensa_intr_enable()

#ifdef __cplusplus
}
#endif

#endif /* NDF_INTR_H */
