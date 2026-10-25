/* GPIO driver: IO_MUX pad config, GPIO matrix, pin interrupts, dedicated GPIO. */
#include "gpio.h"
#include "clk.h"
#include "soc.h"
#include "xtensa.h"

#define GPIO_ENABLE_W1TC   (GPIO_BASE + 0x028)
#define GPIO_ENABLE1_W1TS  (GPIO_BASE + 0x030)
#define GPIO_ENABLE1_W1TC  (GPIO_BASE + 0x034)
#define GPIO_STATUS        (GPIO_BASE + 0x044)
#define GPIO_STATUS_W1TC   (GPIO_BASE + 0x04C)
#define GPIO_STATUS1       (GPIO_BASE + 0x050)
#define GPIO_STATUS1_W1TC  (GPIO_BASE + 0x058)
#define GPIO_PCPU_INT      (GPIO_BASE + 0x05C)
#define GPIO_PCPU_INT1     (GPIO_BASE + 0x068)
#define GPIO_PIN(n)        (GPIO_BASE + 0x074 + (n) * 4)   /* b2 PAD_DRIVER b9:7 INT_TYPE b17:13 INT_ENA */
#define GPIO_FUNC_IN(sig)  (GPIO_BASE + 0x154 + (sig) * 4) /* b5:0 pin b6 inv b7 use-matrix */
#define SRC_GPIO           16
#define RTCIO_PAD(n)       (0x60008484 + 4 * (n))         /* GPIO0..21, b19 RTC mux select */

/* IO_MUX pad bits */
#define MUX_PD     BIT(7)
#define MUX_PU     BIT(8)
#define MUX_IE     BIT(9)
#define MUX_DRV_S  10
#define MUX_SEL_S  12

#define DGPIO_SIG_CORE0  221
#define DGPIO_SIG_CORE1  129

static bool pin_ok(int pin, uint32_t flags)
{
    if (pin < 0 || pin > 48 || (pin >= 22 && pin <= 25)) return false;
    if (flags & GPIO_FORCE) return true;
    return !(pin == 19 || pin == 20 || (pin >= 26 && pin <= 32));
}

int gpio_config(int pin, uint32_t f)
{
    if (!(pin >= 0 && pin <= 48)) return -1;      /* caller index: never past the hardware */
    if (!pin_ok(pin, f)) return -1;
    if (pin <= 21) REG_CLR(RTCIO_PAD(pin), BIT(19));   /* take the pad back from the RTC mux */
    uint32_t mux = 1u << MUX_SEL_S | 2u << MUX_DRV_S;        /* function 1 = GPIO */
    if (f & GPIO_INPUT)    mux |= MUX_IE;
    if (f & GPIO_PULLUP)   mux |= MUX_PU;
    if (f & GPIO_PULLDOWN) mux |= MUX_PD;
    REG(IO_MUX_GPIO(pin)) = mux;
    REG_FIELD(GPIO_PIN(pin), 2, 1, (f & GPIO_OPENDRAIN) ? 1 : 0);
    if (f & GPIO_OUTPUT) {
        REG(GPIO_FUNC_OUT_SEL(pin)) = GPIO_SIG_SIMPLE;
        REG(pin < 32 ? GPIO_ENABLE_W1TS : GPIO_ENABLE1_W1TS) = BIT(pin & 31);
    } else {
        REG(pin < 32 ? GPIO_ENABLE_W1TC : GPIO_ENABLE1_W1TC) = BIT(pin & 31);
    }
    return 0;
}

void gpio_drive(int pin, int s)
{
    if (!(pin >= 0 && pin <= 48)) return;      /* caller index: never past the hardware */
    REG_FIELD(IO_MUX_GPIO(pin), MUX_DRV_S, 2, s & 3);
}

void gpio_out_signal(int pin, int sig, bool inv)
{
    if (!(pin >= 0 && pin <= 48)) return;      /* caller index: never past the hardware */
    /* b10: output-enable comes from GPIO_ENABLE (always driven), not the peripheral */
    REG(GPIO_FUNC_OUT_SEL(pin)) = (sig & 0x1FF) | (inv ? BIT(9) : 0) | BIT(10);
    REG(pin < 32 ? GPIO_ENABLE_W1TS : GPIO_ENABLE1_W1TS) = BIT(pin & 31);
    REG_FIELD(IO_MUX_GPIO(pin), MUX_SEL_S, 3, 1);
}

void gpio_in_signal(int pin, int sig, bool inv)
{
    if (!((pin >= 0 && pin <= 48) || pin == GPIO_IN_HIGH || pin == GPIO_IN_LOW) || sig < 0 || sig > 255) return;   /* pin or the constant-1/0 inputs */
    REG(GPIO_FUNC_IN(sig)) = (pin & 0x3F) | (inv ? BIT(6) : 0) | BIT(7);
    if (pin < 49) REG_SET(IO_MUX_GPIO(pin), MUX_IE);
}

/* ---- pin interrupts ----------------------------------------------------- */
typedef struct { irq_fn_t fn; void *arg; } pin_slot_t;
static pin_slot_t pin_tab[49];
static int gpio_cpu_int[2] = { -1, -1 };

RAM_FN static void gpio_isr(void *unused)
{
    (void)unused;
    uint32_t s0 = REG(GPIO_PCPU_INT), s1 = REG(GPIO_PCPU_INT1);
    REG(GPIO_STATUS_W1TC) = s0;          /* clear first: an edge during the */
    REG(GPIO_STATUS1_W1TC) = s1;         /* handler re-latches, not lost    */
    while (s0) { int p = __builtin_ctz(s0); s0 &= s0 - 1; pin_tab[p].fn(pin_tab[p].arg); }
    while (s1) { int p = __builtin_ctz(s1); s1 &= s1 - 1; pin_tab[p + 32].fn(pin_tab[p + 32].arg); }
}

int gpio_irq_attach(int pin, gpio_irq_t type, int level, irq_fn_t fn, void *arg)
{
    if (!(pin >= 0 && pin <= 48)) return -1;      /* caller index: never past the hardware */
    if (!pin_ok(pin, GPIO_FORCE) || !fn) return -1;
    int core = core_id();
    if (gpio_cpu_int[core] < 0) {
        gpio_cpu_int[core] = irq_attach(SRC_GPIO, level, gpio_isr, 0);
        if (gpio_cpu_int[core] < 0) return -1;
    }
    pin_tab[pin] = (pin_slot_t){ fn, arg };
    REG(pin < 32 ? GPIO_STATUS_W1TC : GPIO_STATUS1_W1TC) = BIT(pin & 31);
    /* INT_ENA b13 routes to the CPU interrupt; the matrix decides which core */
    REG_FIELD(GPIO_PIN(pin), 7, 3, type);
    REG_FIELD(GPIO_PIN(pin), 13, 5, type ? 1 : 0);
    return 0;
}

void gpio_irq_detach(int pin)
{
    REG_FIELD(GPIO_PIN(pin), 13, 5, 0);
    REG_FIELD(GPIO_PIN(pin), 7, 3, 0);
    pin_tab[pin] = (pin_slot_t){ 0, 0 };
}

/* ---- dedicated GPIO ----------------------------------------------------- */
int dgpio_route(int ch, int pin, bool output)
{
    if (ch < 0 || ch > 7 || !pin_ok(pin, 0)) return -1;
    periph_enable(PERIPH_DEDIC_GPIO);
    int sig = (core_id() ? DGPIO_SIG_CORE1 : DGPIO_SIG_CORE0) + ch;
    if (output) {
        gpio_config(pin, GPIO_OUTPUT | GPIO_INPUT);
        REG(GPIO_FUNC_OUT_SEL(pin)) = sig | BIT(10);
    } else {
        gpio_config(pin, GPIO_INPUT);
        gpio_in_signal(pin, sig, false);
    }
    return 0;
}
