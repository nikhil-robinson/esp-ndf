/* UART driver. Registers per TRM ch. 26. */
#include "uart.h"
#include "gpio.h"
#include "clk.h"
#include "irq.h"
#include "xtensa.h"

#define U_BASE(p)     ((p) == 0 ? 0x60000000 : (p) == 1 ? 0x60010000 : 0x6002E000)
#define FIFO(p)       (U_BASE(p) + 0x00)
#define INT_ST(p)     (U_BASE(p) + 0x08)
#define INT_ENA(p)    (U_BASE(p) + 0x0C)
#define INT_CLR(p)    (U_BASE(p) + 0x10)
#define CLKDIV(p)     (U_BASE(p) + 0x14)   /* b11:0 int b23:20 frag */
#define STATUS(p)     (U_BASE(p) + 0x1C)   /* b9:0 rx cnt b25:16 tx cnt */
#define CONF0(p)      (U_BASE(p) + 0x20)
#define CONF1(p)      (U_BASE(p) + 0x24)   /* b9:0 rx full thr b19:10 tx empty thr b23 rx tout en */
#define MEM_CONF(p)   (U_BASE(p) + 0x60)   /* b26:17 rx tout thr (bit times) */
#define CLK_CONF(p)   (U_BASE(p) + 0x78)

#define I_RXFULL  BIT(0)
#define I_TXEMPTY BIT(1)
#define I_RXOVF   BIT(4)
#define I_RXTOUT  BIT(8)
#define I_TXDONE  BIT(14)

#ifndef NDF_UART_RING
#define NDF_UART_RING 2048                            /* per direction, power of 2; an app can lower it (ndf_config.h) */
#endif
#define RING NDF_UART_RING
typedef struct {
    uint8_t rx[RING], tx[RING];
    volatile uint32_t rh, rt, th, tt;                 /* head = write, tail = read */
    volatile uint32_t ovf;
    bool irq_on;
} port_t;
static port_t up[3];

static const periph_t pid[3] = { PERIPH_UART0, PERIPH_UART1, PERIPH_UART2 };
static const uint8_t src[3] = { 27, 28, 29 };
static const uint8_t sig_tx[3] = { 12, 15, 18 }, sig_rx[3] = { 12, 15, 18 };

RAM_FN static void uart_isr(void *arg)
{
    int p = (int)arg;
    port_t *u = &up[p];
    uint32_t st = REG(INT_ST(p));
    if (st & (I_RXFULL | I_RXTOUT | I_RXOVF)) {
        uint32_t n = REG(STATUS(p)) & 0x3FF;
        while (n--) {
            uint8_t b = REG(FIFO(p));
            if (u->rh - u->rt < RING) u->rx[u->rh++ & (RING - 1)] = b;
            else u->ovf++;
        }
    }
    if (st & I_TXEMPTY) {
        uint32_t room = 128 - ((REG(STATUS(p)) >> 16) & 0x3FF);
        while (room-- && u->tt != u->th) REG(FIFO(p)) = u->tx[u->tt++ & (RING - 1)];
        if (u->tt == u->th) REG_CLR(INT_ENA(p), I_TXEMPTY);
    }
    REG(INT_CLR(p)) = st;
}

uint32_t uart_init(int p, const uart_cfg_t *c)
{
    if (p < 0 || p > 2 || !c->baud) return 0;
    /* clkdiv (x16) = 80 MHz * 16 / baud, split over sclk_div if > 12 bits */
    uint32_t sdiv = (80000000 / 4095 + c->baud - 1) / c->baud;      /* >= 1 */
    if (sdiv < 1) sdiv = 1;
    if (sdiv > 256) return 0;
    uint32_t div16 = (uint32_t)(((uint64_t)80000000 * 16 / sdiv + c->baud / 2) / c->baud);
    if ((div16 >> 4) < 1 || (div16 >> 4) > 4095) return 0;

    periph_enable(PERIPH_UART_MEM);
    periph_enable(pid[p]);
    REG(CLK_CONF(p)) = (sdiv - 1) << 12 | 1u << 20 | BIT(22) | BIT(24) | BIT(25);   /* APB, en */
    REG(CLKDIV(p)) = (div16 >> 4) | (div16 & 15) << 20;
    int bits = c->data_bits ? c->data_bits - 5 : 3;
    int stop = c->stop_bits == 2 ? 3 : 1;
    REG(CONF0(p)) = (c->parity & 3) | bits << 2 | stop << 4 | BIT(26) | BIT(28) | BIT(17) | BIT(18);
    REG_CLR(CONF0(p), BIT(17) | BIT(18));                   /* FIFOs out of reset */
    REG(CONF1(p)) = 96 | 32u << 10 | BIT(23);               /* rx full at 96, tx empty at 32, rx timeout on */
    REG_FIELD(MEM_CONF(p), 17, 10, 10);                     /* timeout after 10 idle bit times */

    port_t *u = &up[p];
    u->rh = u->rt = u->th = u->tt = 0; u->ovf = 0;
    if (c->tx_pin >= 0) {
        gpio_config(c->tx_pin, GPIO_OUTPUT | GPIO_INPUT | (p == 0 ? GPIO_FORCE : 0));
        gpio_write(c->tx_pin, 1);
        gpio_out_signal(c->tx_pin, sig_tx[p], false);
    }
    if (c->rx_pin >= 0) {
        gpio_config(c->rx_pin, GPIO_INPUT | GPIO_PULLUP | (p == 0 ? GPIO_FORCE : 0));
        gpio_in_signal(c->rx_pin, sig_rx[p], false);
    }
    REG(INT_CLR(p)) = ~0u;
    if (!u->irq_on) u->irq_on = irq_attach(src[p], 1, uart_isr, (void *)p) >= 0;
    REG(INT_ENA(p)) = I_RXFULL | I_RXTOUT | I_RXOVF;
    return (uint32_t)((uint64_t)80000000 * 16 / sdiv / div16);
}

int uart_write(int p, const void *buf, size_t n)
{
    port_t *u = &up[p];
    const uint8_t *b = buf;
    size_t i = 0;
    if (u->th == u->tt) {                                   /* fast path: straight to FIFO, */
        uint32_t room = 128 - ((REG(STATUS(p)) >> 16) & 0x3FF);   /* one status read per burst */
        while (i < n && room--) REG(FIFO(p)) = b[i++];
    }
    for (; i < n; i++) {
        while (u->th - u->tt >= RING) {}                    /* ring full: wait for the ISR */
        u->tx[u->th & (RING - 1)] = b[i];
        u->th++;
        REG_SET(INT_ENA(p), I_TXEMPTY);
    }
    return n;
}

int uart_read(int p, void *buf, size_t n)
{
    port_t *u = &up[p];
    uint8_t *b = buf;
    size_t k = 0;
    while (k < n && u->rt != u->rh) b[k++] = u->rx[u->rt++ & (RING - 1)];
    return k;
}

size_t   uart_available(int p) { return up[p].rh - up[p].rt; }
uint32_t uart_overflows(int p) { return up[p].ovf; }

void uart_flush(int p)
{
    while (up[p].th != up[p].tt) {}
    while ((REG(STATUS(p)) >> 16) & 0x3FF) {}
    /* last byte still shifting out: one frame (<= 12 bits) */
    uint32_t div16 = (REG(CLKDIV(p)) & 0xFFF) << 4 | ((REG(CLKDIV(p)) >> 20) & 15);
    uint32_t sdiv = ((REG(CLK_CONF(p)) >> 12) & 0xFF) + 1;
    uint32_t cyc = (uint32_t)((uint64_t)12 * div16 * sdiv * 3 / 16);   /* 240/80 = 3 CPU cycles per APB */
    uint32_t t = ccount();
    while (ccount() - t < cyc) {}
}
