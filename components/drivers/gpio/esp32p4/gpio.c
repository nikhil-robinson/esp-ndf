/* P4 GPIO driver, same API as drivers/gpio/gpio.c: IO_MUX pad config, GPIO matrix, pin interrupts, dedicated GPIO.
 * Registers: ESP-IDF v6.1 soc/esp32p4/register/hw_ver3 gpio_reg.h, io_mux_reg.h, lp_iomux_reg.h; GPIO 0-54.
 * Flash and PSRAM have their own MSPI pads on the P4, so no GPIO carries them. */
#include "gpio.h"
#include "clk.h"
#include "soc.h"
#include "cpu.h"

#define NPIN               55
#define GPIO_ENABLE_W1TS   (GPIO_BASE + 0x024)
#define GPIO_ENABLE_W1TC   (GPIO_BASE + 0x028)
#define GPIO_ENABLE1_W1TS  (GPIO_BASE + 0x030)
#define GPIO_ENABLE1_W1TC  (GPIO_BASE + 0x034)
#define GPIO_STATUS_W1TC   (GPIO_BASE + 0x04C)
#define GPIO_STATUS1_W1TC  (GPIO_BASE + 0x058)
#define GPIO_INTR_0        (GPIO_BASE + 0x05C)             /* interrupt status for the CPU (pins 0-31) */
#define GPIO_INTR1_0       (GPIO_BASE + 0x060)             /* pins 32-54 */
#define GPIO_PIN(n)        (GPIO_BASE + 0x074 + (n) * 4)   /* b2 PAD_DRIVER b9:7 INT_TYPE b17:13 INT_ENA */
#define GPIO_FUNC_IN(sig)  (GPIO_BASE + 0x158 + (sig) * 4) /* b5:0 pin b6 inv b7 use-matrix (FUNC1 at 0x15C) */
#define GPIO_FUNC_OUT(n)   (GPIO_BASE + 0x558 + (n) * 4)   /* b8:0 signal b9 inv b10 OE from GPIO_ENABLE */
#define IO_MUX_GPIO(n)     (IO_MUX_BASE + 0x04 + (n) * 4)
#define LP_IOMUX_PAD(n)    (LPPERIPH + 0xB000 + 0x08 + (n) * 4)   /* GPIO0-15: b4 MUX_SEL (1 = LP / analog) */

#define MUX_PD     BIT(7)
#define MUX_PU     BIT(8)
#define MUX_IE     BIT(9)
#define MUX_DRV_S  10
#define MUX_SEL_S  12
#define DGPIO_SIG  214                                     /* CORE_GPIO_OUT_PAD_OUT0 / IN0 */
#define USJ_DM     24                                      /* USB-Serial-JTAG D- / D+: the debug link */
#define USJ_DP     25

static bool pin_ok(int pin, uint32_t flags)
{
    if (pin < 0 || pin >= NPIN) return false;
    if (flags & GPIO_FORCE) return true;
#if NDF_CONSOLE == NDF_CONSOLE_UART0
    if (pin == NDF_CONSOLE_TX_PIN || pin == NDF_CONSOLE_RX_PIN) return false;     /* the console's UART pads */
#endif
    return !(pin == USJ_DM || pin == USJ_DP);
}

int gpio_config(int pin, uint32_t f)
{
    if (!pin_ok(pin, f)) return -1;
    if (pin <= 15) REG_CLR(LP_IOMUX_PAD(pin), BIT(4));    /* take the pad back from the LP mux */
    uint32_t mux = 1u << MUX_SEL_S | 2u << MUX_DRV_S;      /* function 1 = GPIO */
    if (f & GPIO_INPUT)    mux |= MUX_IE;
    if (f & GPIO_PULLUP)   mux |= MUX_PU;
    if (f & GPIO_PULLDOWN) mux |= MUX_PD;
    REG(IO_MUX_GPIO(pin)) = mux;
    REG_FIELD(GPIO_PIN(pin), 2, 1, (f & GPIO_OPENDRAIN) ? 1 : 0);
    if (f & GPIO_OUTPUT) {
        REG(GPIO_FUNC_OUT(pin)) = GPIO_SIG_SIMPLE;
        REG(pin < 32 ? GPIO_ENABLE_W1TS : GPIO_ENABLE1_W1TS) = BIT(pin & 31);
    } else {
        REG(pin < 32 ? GPIO_ENABLE_W1TC : GPIO_ENABLE1_W1TC) = BIT(pin & 31);
    }
    return 0;
}

void gpio_drive(int pin, int s)
{
    if (pin < 0 || pin >= NPIN) return;
    REG_FIELD(IO_MUX_GPIO(pin), MUX_DRV_S, 2, s & 3);
}

void gpio_out_signal(int pin, int sig, bool inv)
{
    if (!pin_ok(pin, GPIO_FORCE) || sig < 0 || sig > 0x1FF) return;
    REG(GPIO_FUNC_OUT(pin)) = (sig & 0x1FF) | (inv ? BIT(9) : 0) | BIT(10);
    REG(pin < 32 ? GPIO_ENABLE_W1TS : GPIO_ENABLE1_W1TS) = BIT(pin & 31);
    REG_FIELD(IO_MUX_GPIO(pin), MUX_SEL_S, 3, 1);
}

void gpio_in_signal(int pin, int sig, bool inv)
{
    if (!((pin >= 0 && pin < NPIN) || pin == GPIO_IN_HIGH || pin == GPIO_IN_LOW) || sig < 1 || sig > 255) return;
    REG(GPIO_FUNC_IN(sig)) = (pin & 0x3F) | (inv ? BIT(6) : 0) | BIT(7);
    if (pin < NPIN) REG_SET(IO_MUX_GPIO(pin), MUX_IE);
}

/* ---- pin interrupts ----------------------------------------------------- */
typedef struct { irq_fn_t fn; void *arg; } pin_slot_t;
static pin_slot_t pin_tab[NPIN];
static int gpio_line[2] = { -1, -1 };

RAM_FN static void gpio_isr(void *unused)
{
    (void)unused;
    uint32_t s0 = REG(GPIO_INTR_0), s1 = REG(GPIO_INTR1_0);
    REG(GPIO_STATUS_W1TC) = s0;
    REG(GPIO_STATUS1_W1TC) = s1;
    while (s0) { int p = __builtin_ctz(s0); s0 &= s0 - 1; if (pin_tab[p].fn) pin_tab[p].fn(pin_tab[p].arg); }
    while (s1) { int p = __builtin_ctz(s1) + 32; s1 &= s1 - 1; if (p < NPIN && pin_tab[p].fn) pin_tab[p].fn(pin_tab[p].arg); }
}

int gpio_irq_attach(int pin, gpio_irq_t type, int level, irq_fn_t fn, void *arg)
{
    if (!pin_ok(pin, GPIO_FORCE) || !fn) return -1;
    int core = core_id();
    if (gpio_line[core] < 0) {
        gpio_line[core] = irq_attach(SRC_GPIO, level, gpio_isr, 0);
        if (gpio_line[core] < 0) return -1;
    }
    pin_tab[pin] = (pin_slot_t){ fn, arg };
    REG(pin < 32 ? GPIO_STATUS_W1TC : GPIO_STATUS1_W1TC) = BIT(pin & 31);
    REG_FIELD(GPIO_PIN(pin), 7, 3, type);
    REG_FIELD(GPIO_PIN(pin), 13, 5, type ? 1 : 0);
    return 0;
}

void gpio_irq_detach(int pin)
{
    if (pin < 0 || pin >= NPIN) return;
    REG_FIELD(GPIO_PIN(pin), 13, 5, 0);
    REG_FIELD(GPIO_PIN(pin), 7, 3, 0);
    pin_tab[pin] = (pin_slot_t){ 0, 0 };
}

/* ---- dedicated GPIO: CPU CSRs 0x803-0x805 (IDF dedic_gpio_cpu_ll.h), matrix signals 214 + ch ------------------ */
int dgpio_route(int ch, int pin, bool output)
{
    if (ch < 0 || ch > 7 || !pin_ok(pin, 0)) return -1;
    if (output) {
        gpio_config(pin, GPIO_OUTPUT | GPIO_INPUT);
        REG(GPIO_FUNC_OUT(pin)) = (DGPIO_SIG + ch) | BIT(10);
        __asm__ volatile("csrc 0x803, %0" :: "r"(BIT(ch)));          /* CSR_GPIO_OEN_USER is active low */
    } else {
        gpio_config(pin, GPIO_INPUT);
        gpio_in_signal(pin, DGPIO_SIG + ch, false);
    }
    return 0;
}
