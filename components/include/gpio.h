/* GPIO: pad config, fast inline I/O, GPIO-matrix routing, pin interrupts,
 * dedicated (single-cycle, CPU-instruction) GPIO. */
#pragma once
#include "ndf.h"
#include "irq.h"

enum {
    GPIO_INPUT     = BIT(0),
    GPIO_OUTPUT    = BIT(1),
    GPIO_OPENDRAIN = BIT(2),
    GPIO_PULLUP    = BIT(3),
    GPIO_PULLDOWN  = BIT(4),
    GPIO_FORCE     = BIT(7),   /* allow console (19/20) and flash (26-32) pins */
};

typedef enum {
    GPIO_IRQ_NONE = 0, GPIO_IRQ_RISING = 1, GPIO_IRQ_FALLING = 2, GPIO_IRQ_ANY = 3,
    GPIO_IRQ_LOW = 4, GPIO_IRQ_HIGH = 5,
} gpio_irq_t;

#define GPIO_SIG_SIMPLE  0x100    /* output signal: plain GPIO (gpio_write) */
#define GPIO_IN_HIGH     0x38     /* input "pin": constant 1 */
#define GPIO_IN_LOW      0x3C     /* input "pin": constant 0 */

int  gpio_config(int pin, uint32_t flags);   /* -1 if pin invalid/reserved */
void gpio_drive(int pin, int strength);      /* 0..3 (~5, 10, 20, 40 mA)  */
void gpio_out_signal(int pin, int sig, bool invert);   /* peripheral -> pin */
void gpio_in_signal(int pin, int sig, bool invert);    /* pin -> peripheral */

/* Pin interrupts. All pins share one CPU interrupt per core (set by first
 * attach on that core, at `level`). Handler runs on the attaching core. */
int  gpio_irq_attach(int pin, gpio_irq_t type, int level, irq_fn_t fn, void *arg);
void gpio_irq_detach(int pin);

/* ---- fast path: inline register access ---------------------------------- */
#define GPIO_REG(o)  (*(volatile uint32_t *)(0x60004000 + (o)))

static inline void gpio_set(int pin)
{
    if (pin < 32) GPIO_REG(0x08) = BIT(pin); else GPIO_REG(0x14) = BIT(pin - 32);
}
static inline void gpio_clr(int pin)
{
    if (pin < 32) GPIO_REG(0x0C) = BIT(pin); else GPIO_REG(0x18) = BIT(pin - 32);
}
static inline void gpio_write(int pin, bool v) { if (v) gpio_set(pin); else gpio_clr(pin); }
static inline bool gpio_read(int pin)
{
    return pin < 32 ? (GPIO_REG(0x3C) >> pin) & 1 : (GPIO_REG(0x40) >> (pin - 32)) & 1;
}
/* read-modify-write: not atomic against an ISR writing the same bank */
static inline void gpio_toggle(int pin)
{
    if (pin < 32) GPIO_REG(0x04) ^= BIT(pin); else GPIO_REG(0x10) ^= BIT(pin - 32);
}

/* ---- dedicated GPIO: 8 channels per core, driven by CPU instructions ----
 * ~1 cycle per update vs ~10+ for a bus write. Route with dgpio_route()
 * on the core that will drive the channel. */
int  dgpio_route(int ch, int pin, bool output);   /* calling core's channel */

static inline void dgpio_write(uint32_t mask, uint32_t val)
{
    __asm__ volatile("ee.wr_mask_gpio_out %0, %1" :: "r"(val), "r"(mask));
}
static inline uint32_t dgpio_read(void)
{
    uint32_t v;
    __asm__ volatile("ee.get_gpio_in %0" : "=r"(v));
    return v;
}
