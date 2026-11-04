/* UART driver. Registers per TRM ch. 26. */
#include "uart.h"
#include "spin.h"
#include "gpio.h"
#include "clk.h"
#include "irq.h"
#include "xtensa.h"
#include "gdma.h"
#include "sys.h"

#define U_BASE(p)     ((p) == 0 ? 0x60000000 : (p) == 1 ? 0x60010000 : 0x6002E000)
#define FIFO(p)       (U_BASE(p) + 0x00)
#define INT_ST(p)     (U_BASE(p) + 0x08)
#define INT_ENA(p)    (U_BASE(p) + 0x0C)
#define INT_CLR(p)    (U_BASE(p) + 0x10)
#define CLKDIV(p)     (U_BASE(p) + 0x14)   /* b11:0 int b23:20 frag */
#define STATUS(p)     (U_BASE(p) + 0x1C)   /* b9:0 rx cnt b25:16 tx cnt */
#define CONF0(p)      (U_BASE(p) + 0x20)
#define CONF1(p)      (U_BASE(p) + 0x24)   /* b9:0 rx full thr b19:10 tx empty thr b23 rx tout en */
#define RS485(p)      (U_BASE(p) + 0x4C)   /* b0 en b3 tx->rx loop b4 tx while rx busy */
#define AT_PRE(p)     (U_BASE(p) + 0x50)
#define AT_POST(p)    (U_BASE(p) + 0x54)
#define AT_GAP(p)     (U_BASE(p) + 0x58)
#define AT_CHAR(p)    (U_BASE(p) + 0x5C)   /* b7:0 char b15:8 count */
#define MEM_CONF(p)   (U_BASE(p) + 0x60)   /* b16:7 rx flow thr b26:17 rx tout thr (bit times) */
#define FSM(p)        (U_BASE(p) + 0x6C)   /* b7:4 tx state, 0 = idle */
#define CLK_CONF(p)   (U_BASE(p) + 0x78)

#define I_RXFULL  BIT(0)
#define I_TXEMPTY BIT(1)
#define I_RXOVF   BIT(4)
#define I_RXTOUT  BIT(8)
#define I_TXDONE  BIT(14)
#define I_CLASH   (BIT(15) | BIT(16) | BIT(17))      /* rs485 parity / frame error / clash */
#define I_AT      BIT(18)
#define C0_SW_RTS BIT(6)

#define RING NDF_UART_RING                            /* per direction, power of 2 (ndf_config.h) */
typedef struct {
    uint8_t rx[RING], tx[RING];
    volatile uint32_t rh, rt, th, tt;                 /* head = write, tail = read */
    volatile uint32_t ovf, clash;
    volatile uint32_t ph, pt, pat[8];                 /* pattern end positions (ring index), queue */
    uint8_t pat_n, mode;
    bool irq_on;
} port_t;
static port_t up[3];

static const periph_t pid[3] = { PERIPH_UART0, PERIPH_UART1, PERIPH_UART2 };
static const uint8_t src[3] = { 27, 28, 29 };
static const uint8_t sig_tx[3] = { 12, 15, 18 }, sig_rx[3] = { 12, 15, 18 }, sig_rts[3] = { 13, 16, 19 };

RAM_FN static void uart_isr(void *arg)
{
    int p = (int)arg;
    port_t *u = &up[p];
    uint32_t st = REG(INT_ST(p));
    if (st & (I_RXFULL | I_RXTOUT | I_RXOVF | I_AT)) {
        uint32_t n = REG(STATUS(p)) & 0x3FF;
        while (n--) {
            uint8_t b = REG(FIFO(p));
            if (u->rh - u->rt < RING) u->rx[u->rh++ & (RING - 1)] = b;
            else u->ovf++;
        }
        if ((st & I_AT) && u->ph - u->pt < 8) u->pat[u->ph++ & 7] = u->rh - u->pat_n;   /* pattern start */
    }
    if (st & I_CLASH) {                               /* IDF: drop what the collision garbled */
        u->clash++;
        REG_SET(CONF0(p), BIT(17)); REG_CLR(CONF0(p), BIT(17));
    }
    if ((st & I_TXDONE) && u->tt == u->th && !((REG(STATUS(p)) >> 16) & 0x3FF) && !((REG(FSM(p)) >> 4) & 15)) {
        REG_CLR(INT_ENA(p), I_TXDONE);                /* half duplex: frame out, release the line */
        if (u->mode == UART_MODE_IRDA) REG_CLR(CONF0(p), BIT(10));          /* irda_tx_en off: receiver back */
        else {                                        /* RS485: drop the echo, DE low */
            REG_SET(CONF0(p), BIT(17)); REG_CLR(CONF0(p), BIT(17));
            REG_SET(CONF0(p), C0_SW_RTS);
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
    if (!(p >= 0 && p < 3)) return 0;      /* caller index: never past the hardware */
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
    u->rh = u->rt = u->th = u->tt = 0; u->ovf = u->clash = 0; u->ph = u->pt = 0; u->mode = UART_MODE_UART;
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
    if (!(p >= 0 && p < 3)) return -1;      /* caller index: never past the hardware */
    port_t *u = &up[p];
    const uint8_t *b = buf;
    size_t i = 0;
    if (u->mode == UART_MODE_RS485 || u->mode == UART_MODE_IRDA) {   /* IDF: drive DE, then arm TX_DONE */
        uint32_t ps = irq_lock();
        if (u->mode == UART_MODE_IRDA) REG_SET(CONF0(p), BIT(10));      /* irda_tx_en: TX on, RX blind */
        else REG_CLR(CONF0(p), C0_SW_RTS);
        REG(INT_CLR(p)) = I_TXDONE;
        REG_SET(INT_ENA(p), I_TXDONE);
        irq_unlock(ps);
    }
    if (u->th == u->tt) {                                   /* fast path: straight to FIFO, */
        uint32_t room = 128 - ((REG(STATUS(p)) >> 16) & 0x3FF);   /* one status read per burst */
        while (i < n && room--) REG(FIFO(p)) = b[i++];
    }
    for (; i < n; i++) {
        uint32_t tt0 = u->tt, t0 = ccount();                /* ring full: wait for the ISR, but not for a dead one */
        while (u->th - u->tt >= RING) {
            if (u->tt != tt0) { tt0 = u->tt; t0 = ccount(); }
            else if (ccount() - t0 > 240000000u) return (int)i;   /* 1 s without progress: UART stopped */
        }
        u->tx[u->th & (RING - 1)] = b[i];
        u->th++;
        REG_SET(INT_ENA(p), I_TXEMPTY);
    }
    return n;
}

int uart_read(int p, void *buf, size_t n)
{
    if (!(p >= 0 && p < 3)) return -1;      /* caller index: never past the hardware */
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
    for (uint32_t tt0 = up[p].tt, t0 = ccount(); up[p].th != up[p].tt; )      /* 1 s without progress: give up */
        if (up[p].tt != tt0) { tt0 = up[p].tt; t0 = ccount(); } else if (ccount() - t0 > 240000000u) return;
    for (uint32_t f0 = ~0u, t0 = ccount(), f; (f = (REG(STATUS(p)) >> 16) & 0x3FF); )
        if (f != f0) { f0 = f; t0 = ccount(); } else if (ccount() - t0 > 240000000u) return;
    /* last byte still shifting out: one frame (<= 12 bits) */
    uint32_t div16 = (REG(CLKDIV(p)) & 0xFFF) << 4 | ((REG(CLKDIV(p)) >> 20) & 15);
    uint32_t sdiv = ((REG(CLK_CONF(p)) >> 12) & 0xFF) + 1;
    uint32_t cyc = (uint32_t)((uint64_t)12 * div16 * sdiv * 3 / 16);   /* 240/80 = 3 CPU cycles per APB */
    uint32_t t = ccount();
    while (ccount() - t < cyc) {}
}

int uart_set_flow(int p, int rts_pin, int cts_pin, uint32_t rx_thresh)
{
    if (!(p >= 0 && p < 3)) return -1;      /* caller index: never past the hardware */
    if (p < 0 || p > 2 || rx_thresh > 127) return -1;
    if (cts_pin >= 0) {                                      /* first: RTS may share the pad (loop test) */
        gpio_config(cts_pin, GPIO_INPUT | GPIO_PULLUP);
        gpio_in_signal(cts_pin, sig_rts[p], false);          /* UxCTS_IN has the same index as UxRTS_OUT */
        REG_SET(CONF0(p), BIT(15));
    } else REG_CLR(CONF0(p), BIT(15));
    if (rts_pin >= 0) {
        gpio_config(rts_pin, GPIO_OUTPUT | GPIO_INPUT);
        gpio_out_signal(rts_pin, sig_rts[p], false);
        REG_FIELD(MEM_CONF(p), 7, 10, rx_thresh ? rx_thresh : 100);
        REG_SET(CONF1(p), BIT(22));
    } else REG_CLR(CONF1(p), BIT(22));
    return 0;
}

int uart_set_mode(int p, int mode, int de_pin)
{
    if (!(p >= 0 && p < 3)) return -1;      /* caller index: never past the hardware */
    if (p < 0 || p > 2) return -1;
    port_t *u = &up[p];
    uint32_t ps = irq_lock();
    REG(RS485(p)) = 0;
    REG_CLR(CONF0(p), BIT(16) | BIT(10) | C0_SW_RTS);
    REG_CLR(INT_ENA(p), I_CLASH | I_TXDONE);
    switch (mode) {                                         /* register sets as IDF uart_ll_set_mode_* */
    case UART_MODE_RS485:   REG_SET(CONF0(p), C0_SW_RTS); REG(RS485(p)) = BIT(0) | BIT(4); break;
    case UART_MODE_RS485_CD:
        REG(RS485(p)) = BIT(0) | BIT(3) | BIT(4);
        REG(INT_CLR(p)) = I_CLASH; REG_SET(INT_ENA(p), I_CLASH); break;
    case UART_MODE_IRDA:    REG_SET(CONF0(p), BIT(16)); break;   /* irda_en; half duplex: RX blind while TX */
    default: mode = UART_MODE_UART;
    }
    u->mode = mode;
    irq_unlock(ps);
    if (de_pin >= 0) { gpio_config(de_pin, GPIO_OUTPUT | GPIO_INPUT); gpio_out_signal(de_pin, sig_rts[p], false); }
    return 0;
}

uint32_t uart_rs485_clashes(int p) { return up[p].clash; }

int uart_set_pattern(int p, char c, int n, uint32_t gap, uint32_t pre, uint32_t post)
{
    if (!(p >= 0 && p < 3)) return -1;      /* caller index: never past the hardware */
    if (p < 0 || p > 2 || n < 0 || n > 255) return -1;
    port_t *u = &up[p];
    if (!n) { REG_CLR(INT_ENA(p), I_AT); return 0; }          /* n = 0: off */
    REG(AT_CHAR(p)) = (uint8_t)c | (uint32_t)n << 8;
    REG(AT_GAP(p)) = gap; REG(AT_PRE(p)) = pre; REG(AT_POST(p)) = post;   /* in baud cycles, as IDF */
    u->pat_n = n; u->ph = u->pt = 0;
    REG(INT_CLR(p)) = I_AT;
    REG_SET(INT_ENA(p), I_AT);
    return 0;
}

int uart_pattern_pop(int p)
{
    port_t *u = &up[p];
    if (u->ph == u->pt) return -1;
    int pos = (int)(u->pat[u->pt++ & 7] - u->rt);            /* offset from the next byte uart_read returns */
    return pos;
}

/* ---- DMA through UHCI0 (one UART at a time), transparent: no SLIP escapes, no packet header (UHCI CONF0
 * head_en / seper_en off, ESCAPE_CONF 0). TX = one GDMA chain; RX ends at line idle (IDLE_CONF threshold,
 * uart_idle_eof_en) or when the buffer is full. The UART's own RX interrupt is off while DMA owns the port. */
#define UHCI_CONF0   0x60014000
#define UHCI_CONF1   0x60014018
#define UHCI_ESCAPE  0x60014024
#define IDLE_CONF(p) (U_BASE(p) + 0x48)              /* b9:0 rx idle threshold (bit times) */
static int uh_port = -1, uh_ch = -1;
static gdma_desc_t uh_txd[8], uh_rxd[8];

int uart_dma_start(int p, uint32_t idle_bits)
{
    if (!(p >= 0 && p < 3)) return -1;      /* caller index: never past the hardware */
    if (p < 0 || p > 2 || uh_port >= 0) return -1;
    if (uh_ch < 0 && (uh_ch = gdma_alloc()) < 0) return -1;
    periph_enable(PERIPH_UHCI0);
    gdma_connect(uh_ch, GDMA_UHCI0);
    REG(UHCI_CONF0) = BIT(11);                       /* clk_en (IDF uhci_ll_init) */
    REG(UHCI_CONF0) = BIT(11) | BIT(2 + p) | BIT(8); /* uartN_ce, idle eof */
    REG(UHCI_CONF1) = 0;
    REG(UHCI_ESCAPE) = 0;
    REG_FIELD(IDLE_CONF(p), 0, 10, idle_bits ? idle_bits : 20);
    REG_CLR(INT_ENA(p), I_RXFULL | I_RXTOUT | I_RXOVF);
    uh_port = p;
    return 0;
}

void uart_dma_stop(int p)
{
    if (p != uh_port) return;
    gdma_reset(uh_ch);
    REG(UHCI_CONF0) = BIT(11);
    REG(INT_CLR(p)) = ~0u;
    REG_SET(INT_ENA(p), I_RXFULL | I_RXTOUT | I_RXOVF);
    uh_port = -1;
}

int uart_dma_write(int p, const void *buf, size_t n, uint32_t timeout_us)
{
    if (gdma_is_psram(buf)) return -1;                          /* TX DMA from PSRAM: refused (gdma.h) */
    if (!(p >= 0 && p < 3)) return -1;      /* caller index: never past the hardware */
    if (p != uh_port || gdma_chain(uh_txd, 8, (void *)buf, n, true) < 0) return -1;
    gdma_out_reset(uh_ch);
    gdma_out_start(uh_ch, uh_txd);
    if (!timeout_us) return n;                        /* async: poll uart_dma_write_done() */
    uint32_t t0 = ccount(), lim = timeout_us * (sys_cpu_hz() / 1000000);
    while (!gdma_out_eof(uh_ch)) if (ccount() - t0 > lim) return -1;
    return n;                                         /* in the UART FIFO; uart_flush waits for the wire */
}

bool uart_dma_write_done(int p) { return p == uh_port && gdma_out_eof(uh_ch); }

int uart_dma_read_start(int p, void *buf, size_t max)
{
    if (!(p >= 0 && p < 3)) return -1;      /* caller index: never past the hardware */
    if (p != uh_port || gdma_chain(uh_rxd, 8, buf, max, false) < 0) return -1;
    gdma_in_reset(uh_ch);
    gdma_in_start(uh_ch, uh_rxd);
    return 0;
}

int uart_dma_read_done(int p)                         /* bytes once the frame ended (idle / full), -1 before */
{
    if (p != uh_port || !gdma_in_eof(uh_ch)) {
        if (p == uh_port) {                           /* buffer full also ends it */
            gdma_desc_t *d = uh_rxd; while (d->next) d = d->next;
            if (d->owner) return -1;
        } else return -1;
    }
    int n = 0;
    for (gdma_desc_t *d = uh_rxd; d && !d->owner; d = d->next) n += d->length;
    return n;
}

