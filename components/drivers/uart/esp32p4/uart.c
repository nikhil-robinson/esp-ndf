/* P4 UART driver, same API as drivers/uart/uart.c. UART0-4 (ESP-IDF v6.1 hw_ver3 uart_reg.h): the UART core runs in
 * its own clock domain, so configuration writes take effect on UART_REG_UPDATE; FIFO 128 bytes, 8-bit counts.
 * Core clock = XTAL 40 MHz (HP_SYS_CLKRST PERI_CLK_CTRL110 + 4 n), independent of the CPU frequency.
 * Interrupt-driven RX and TX rings as on the S3, plus the same extras: RTS/CTS flow control, RS485 (+ collision detect),
 * IrDA, AT-pattern detection and DMA through UHCI0 (ESP-IDF v6.1 uart_ll.h / uhci_ll.h; the sync registers take effect
 * on REG_UPDATE). */
#include "uart.h"
#include "spin.h"
#include "gpio.h"
#include "clk.h"
#include "irq.h"
#include "cpu.h"
#include "sys.h"
#include "soc.h"
#include "gdma.h"
#include "cache_p4.h"

#define NPORT          5
#define U_BASE(p)      (UART_BASE(p))
#define FIFO(p)        (U_BASE(p) + 0x00)
#define INT_ST(p)      (U_BASE(p) + 0x08)
#define INT_ENA(p)     (U_BASE(p) + 0x0C)
#define INT_CLR(p)     (U_BASE(p) + 0x10)
#define CLKDIV(p)      (U_BASE(p) + 0x14)   /* b11:0 int b23:20 frag */
#define STATUS(p)      (U_BASE(p) + 0x1C)   /* b7:0 rx cnt b23:16 tx cnt */
#define CONF0(p)       (U_BASE(p) + 0x20)   /* b1:0 parity b3:2 bits b5:4 stop b18 err_wr_mask b22 rxrst b23 txrst */
#define CONF1(p)       (U_BASE(p) + 0x24)   /* b7:0 rx full thr b15:8 tx empty thr */
#define HWFC_CONF(p)   (U_BASE(p) + 0x2C)   /* b7:0 rx flow thr b8 rx flow en */
#define IDLE_CONF(p)   (U_BASE(p) + 0x48)   /* b9:0 rx idle threshold (bit times) */
#define RS485(p)       (U_BASE(p) + 0x4C)   /* b0 en b3 tx->rx loop b4 tx while rx busy */
#define AT_PRE(p)      (U_BASE(p) + 0x50)
#define AT_POST(p)     (U_BASE(p) + 0x54)
#define AT_GAP(p)      (U_BASE(p) + 0x58)
#define AT_CHAR(p)     (U_BASE(p) + 0x5C)   /* b7:0 char b15:8 count */
#define TOUT_CONF(p)   (U_BASE(p) + 0x64)   /* b0 rx timeout en b11:2 thr (bit times) */
#define FSM(p)         (U_BASE(p) + 0x70)   /* b7:4 tx state, 0 = idle */
#define CLK_CONF(p)    (U_BASE(p) + 0x88)   /* b24 tx sclk en b25 rx sclk en (IDF uart_ll_sclk_enable) */
#define REG_UPDATE(p)  (U_BASE(p) + 0x98)
#define CLK_SEL(p)     (HP_CLKRST_BASE + 0x68 + 4 * (p))   /* b25:24 src (0 XTAL) b26 en */
#define CLK_DIV(p)     (HP_CLKRST_BASE + 0x6C + 4 * (p))   /* b7:0 int div - 1, b15:8 num, b23:16 den */
#define SCLK_HZ        40000000u

#define I_RXFULL  BIT(0)
#define I_TXEMPTY BIT(1)
#define I_RXOVF   BIT(4)
#define I_RXTOUT  BIT(8)
#define I_TXDONE  BIT(14)
#define I_CLASH   (BIT(15) | BIT(16) | BIT(17))      /* rs485 parity / frame error / clash */
#define I_AT      BIT(18)
#define C0_TX_FLOW BIT(13)
#define C0_IRDA   BIT(14)
#define C0_IRDA_TX BIT(8)
#define C0_SW_RTS BIT(21)
#define C0_RXRST  BIT(22)
#define C0_TXRST  BIT(23)

#define RING NDF_UART_RING
typedef struct {
    uint8_t rx[RING], tx[RING];
    volatile uint32_t rh, rt, th, tt;
    volatile uint32_t ovf, clash;
    volatile uint32_t ph, pt, pat[8];                 /* pattern end positions (ring index), queue */
    uint8_t pat_n, mode;
    bool irq_on;
} port_t;
static port_t up[NPORT];

static const periph_t pid[2] = { PERIPH_UART0, PERIPH_UART1 };

static void update(int p)
{
    REG(REG_UPDATE(p)) = 1;
    SPIN_US(!(REG(REG_UPDATE(p)) & 1), 1000);
}

RAM_FN static void uart_isr(void *arg)
{
    int p = (int)arg;
    port_t *u = &up[p];
    uint32_t st = REG(INT_ST(p));
    if (st & (I_RXFULL | I_RXTOUT | I_RXOVF | I_AT)) {
        uint32_t n = REG(STATUS(p)) & 0xFF;
        while (n--) {
            uint8_t b = REG(FIFO(p));
            if (u->rh - u->rt < RING) u->rx[u->rh++ & (RING - 1)] = b;
            else u->ovf++;
        }
        if ((st & I_AT) && u->ph - u->pt < 8) u->pat[u->ph++ & 7] = u->rh - u->pat_n;   /* pattern start */
    }
    if (st & I_CLASH) {                               /* IDF: drop what the collision garbled */
        u->clash++;
        REG_SET(CONF0(p), C0_RXRST); REG_CLR(CONF0(p), C0_RXRST);
    }
    if ((st & I_TXDONE) && u->tt == u->th && !((REG(STATUS(p)) >> 16) & 0xFF) && !((REG(FSM(p)) >> 4) & 15)) {
        REG_CLR(INT_ENA(p), I_TXDONE);                /* half duplex: frame out, release the line */
        if (u->mode == UART_MODE_IRDA) REG_CLR(CONF0(p), C0_IRDA_TX);      /* receiver back */
        else {                                        /* RS485: drop the echo, DE low */
            REG_SET(CONF0(p), C0_RXRST); REG_CLR(CONF0(p), C0_RXRST);
            REG_SET(CONF0(p), C0_SW_RTS);
        }
        update(p);
    }
    if (st & I_TXEMPTY) {
        uint32_t room = 128 - ((REG(STATUS(p)) >> 16) & 0xFF);
        while (room-- && u->tt != u->th) REG(FIFO(p)) = u->tx[u->tt++ & (RING - 1)];
        if (u->tt == u->th) REG_CLR(INT_ENA(p), I_TXEMPTY);
    }
    REG(INT_CLR(p)) = st;
}

uint32_t uart_init(int p, const uart_cfg_t *c)
{
    if (p < 0 || p >= NPORT || !c->baud) return 0;
    uint32_t div16 = (uint32_t)(((uint64_t)SCLK_HZ * 16 + c->baud / 2) / c->baud);   /* sclk div 1 */
    if ((div16 >> 4) < 1 || (div16 >> 4) > 4095) return 0;

    if (p <= 1) periph_enable(pid[p]);
    else {                                  /* UART2-4: APB gate (SOC_CLK_CTRL1 b7+n) and resets (HP_RST_EN1 b8+n core, b13+n apb), like UART0/1 */
        REG_SET(HP_CLKRST_BASE + 0x1C, BIT(7 + p));
        REG_CLR(HP_CLKRST_BASE + 0xC4, BIT(8 + p) | BIT(13 + p));
    }
    REG(CLK_SEL(p)) = (REG(CLK_SEL(p)) & ~(3u << 24)) | BIT(26);   /* XTAL, clock on */
    REG_FIELD(CLK_DIV(p), 0, 24, 0);
    REG_SET(CLK_CONF(p), BIT(24) | BIT(25));
    REG(CLKDIV(p)) = (div16 >> 4) | (div16 & 15) << 20;
    int bits = c->data_bits ? c->data_bits - 5 : 3;
    int stop = c->stop_bits == 2 ? 3 : 1;
    REG(CONF0(p)) = (c->parity & 3) | bits << 2 | stop << 4 | BIT(18) | BIT(22) | BIT(23);
    update(p);
    REG_CLR(CONF0(p), BIT(22) | BIT(23));
    REG(CONF1(p)) = 96 | 32u << 8;                          /* rx full at 96, tx empty at 32 */
    REG(TOUT_CONF(p)) = BIT(0) | 10u << 2;                   /* rx timeout after 10 idle bit times */
    update(p);

    port_t *u = &up[p];
    u->rh = u->rt = u->th = u->tt = 0; u->ovf = u->clash = 0; u->ph = u->pt = 0; u->mode = UART_MODE_UART;
    if (c->tx_pin >= 0) {
        gpio_config(c->tx_pin, GPIO_OUTPUT | GPIO_INPUT | (p == 0 ? GPIO_FORCE : 0));
        gpio_write(c->tx_pin, 1);
        gpio_out_signal(c->tx_pin, 10 + 3 * p, false);       /* UARTn_TXD_PAD_OUT_IDX */
    }
    if (c->rx_pin >= 0) {
        if (!gpio_is_output(c->rx_pin)) gpio_config(c->rx_pin, GPIO_INPUT | GPIO_PULLUP | (p == 0 ? GPIO_FORCE : 0));   /* RX on the TX pad: loopback without a jumper */
        gpio_in_signal(c->rx_pin, 10 + 3 * p, false);        /* UARTn_RXD_PAD_IN_IDX */
    }
    REG(INT_CLR(p)) = ~0u;
    if (!u->irq_on) u->irq_on = irq_attach(SRC_UART0 + p, 1, uart_isr, (void *)p) >= 0;
    REG(INT_ENA(p)) = I_RXFULL | I_RXTOUT | I_RXOVF;
    if (c->rx_pin >= 0) {   /* switching the pads on makes the receiver see a low for a moment: a stray byte (measured). Let a frame time pass, then drop it */
        uint32_t baud = (uint32_t)((uint64_t)SCLK_HZ * 16 / div16), us = 12u * 1000000u / (baud ? baud : 1);
        delay_us(us > 300 ? 300 : us + 1);
        uint32_t ps = irq_lock();
        REG_SET(CONF0(p), C0_RXRST); REG_CLR(CONF0(p), C0_RXRST); update(p);
        REG(INT_CLR(p)) = ~0u; u->rh = u->rt = 0; u->ovf = 0;
        irq_unlock(ps);
    }
    return (uint32_t)((uint64_t)SCLK_HZ * 16 / div16);
}

int uart_write(int p, const void *buf, size_t n)
{
    if (p < 0 || p >= NPORT) return -1;
    port_t *u = &up[p];
    const uint8_t *b = buf;
    size_t i = 0;
    if (n && (u->mode == UART_MODE_RS485 || u->mode == UART_MODE_IRDA)) {      /* half duplex: DE high / receiver off until TX done */
        if (u->mode == UART_MODE_IRDA) REG_SET(CONF0(p), C0_IRDA_TX); else REG_CLR(CONF0(p), C0_SW_RTS);
        update(p);
        REG(INT_CLR(p)) = I_TXDONE; REG_SET(INT_ENA(p), I_TXDONE);
    }
    if (u->th == u->tt) {
        uint32_t room = 128 - ((REG(STATUS(p)) >> 16) & 0xFF);
        while (i < n && room--) REG(FIFO(p)) = b[i++];
    }
    uint32_t sec = sys_cpu_hz();
    for (; i < n; i++) {
        uint32_t tt0 = u->tt, t0 = ccount();
        while (u->th - u->tt >= RING) {
            if (u->tt != tt0) { tt0 = u->tt; t0 = ccount(); }
            else if (ccount() - t0 > sec) return (int)i;       /* 1 s without progress: UART stopped */
        }
        u->tx[u->th & (RING - 1)] = b[i];
        u->th++;
        REG_SET(INT_ENA(p), I_TXEMPTY);
    }
    return n;
}

int uart_read(int p, void *buf, size_t n)
{
    if (p < 0 || p >= NPORT) return -1;
    port_t *u = &up[p];
    uint8_t *b = buf;
    size_t k = 0;
    while (k < n && u->rt != u->rh) b[k++] = u->rx[u->rt++ & (RING - 1)];
    return k;
}

size_t   uart_available(int p) { return p >= 0 && p < NPORT ? up[p].rh - up[p].rt : 0; }
uint32_t uart_overflows(int p) { return p >= 0 && p < NPORT ? up[p].ovf : 0; }

void uart_flush(int p)
{
    if (p < 0 || p >= NPORT) return;
    uint32_t sec = sys_cpu_hz();
    for (uint32_t tt0 = up[p].tt, t0 = ccount(); up[p].th != up[p].tt; )
        if (up[p].tt != tt0) { tt0 = up[p].tt; t0 = ccount(); } else if (ccount() - t0 > sec) return;
    for (uint32_t f0 = ~0u, t0 = ccount(), f; (f = (REG(STATUS(p)) >> 16) & 0xFF); )
        if (f != f0) { f0 = f; t0 = ccount(); } else if (ccount() - t0 > sec) return;
    SPIN_US(!((REG(FSM(p)) >> 4) & 15), 10000);              /* last frame shifting out */
}

static const uint8_t sig_rts[NPORT] = { 11, 14, 17, 20, 23 };           /* UARTn_RTS_PAD_OUT_IDX = UARTn_CTS_PAD_IN_IDX */

int uart_set_flow(int p, int rts_pin, int cts_pin, uint32_t rx_thresh)
{
    if (p < 0 || p >= NPORT || rx_thresh > 127) return -1;
    if (cts_pin >= 0) {                                      /* first: RTS may share the pad (loop test) */
        gpio_config(cts_pin, GPIO_INPUT | GPIO_PULLUP);
        gpio_in_signal(cts_pin, sig_rts[p], false);
        REG_SET(CONF0(p), C0_TX_FLOW);
    } else REG_CLR(CONF0(p), C0_TX_FLOW);
    if (rts_pin >= 0) {
        gpio_config(rts_pin, GPIO_OUTPUT | GPIO_INPUT);
        gpio_out_signal(rts_pin, sig_rts[p], false);
        REG(HWFC_CONF(p)) = BIT(8) | (rx_thresh ? rx_thresh : 100);
    } else REG_CLR(HWFC_CONF(p), BIT(8));
    update(p);
    return 0;
}

int uart_set_mode(int p, int mode, int de_pin)
{
    if (p < 0 || p >= NPORT) return -1;
    port_t *u = &up[p];
    uint32_t ps = irq_lock();
    REG(RS485(p)) = 0;
    REG_CLR(CONF0(p), C0_IRDA | C0_IRDA_TX | C0_SW_RTS);
    REG_CLR(INT_ENA(p), I_CLASH | I_TXDONE);
    switch (mode) {                                         /* register sets as IDF uart_ll_set_mode_* */
    case UART_MODE_RS485:   REG_SET(CONF0(p), C0_SW_RTS); REG(RS485(p)) = BIT(0) | BIT(1) | BIT(2); break;       /* uart_ll_set_mode_rs485_half_duplex: en, dl0, dl1; the receiver is blind while TX runs */
    case UART_MODE_RS485_CD:
        REG(RS485(p)) = BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4);                 /* uart_ll_set_mode_collision_detect */
        REG(INT_CLR(p)) = I_CLASH; REG_SET(INT_ENA(p), I_CLASH); break;
    case UART_MODE_IRDA:    REG_SET(CONF0(p), C0_IRDA); break;   /* half duplex: RX blind while TX */
    default: mode = UART_MODE_UART;
    }
    update(p);
    u->mode = (uint8_t)mode;
    irq_unlock(ps);
    if (de_pin >= 0) { gpio_config(de_pin, GPIO_OUTPUT | GPIO_INPUT); gpio_out_signal(de_pin, sig_rts[p], false); }
    return 0;
}

uint32_t uart_rs485_clashes(int p) { return p >= 0 && p < NPORT ? up[p].clash : 0; }

int uart_set_pattern(int p, char c, int n, uint32_t gap, uint32_t pre, uint32_t post)
{
    if (p < 0 || p >= NPORT || n < 0 || n > 255) return -1;
    port_t *u = &up[p];
    if (!n) { REG_CLR(INT_ENA(p), I_AT); return 0; }          /* n = 0: off */
    REG(AT_CHAR(p)) = (uint8_t)c | (uint32_t)n << 8;
    REG(AT_GAP(p)) = gap; REG(AT_PRE(p)) = pre; REG(AT_POST(p)) = post;   /* in baud cycles, as IDF */
    update(p);
    u->pat_n = (uint8_t)n; u->ph = u->pt = 0;
    REG(INT_CLR(p)) = I_AT;
    REG_SET(INT_ENA(p), I_AT);
    return 0;
}

int uart_pattern_pop(int p)
{
    if (p < 0 || p >= NPORT) return -1;
    port_t *u = &up[p];
    if (u->ph == u->pt) return -1;
    return (int)(u->pat[u->pt++ & 7] - u->rt);               /* offset from the next byte uart_read returns */
}

/* ---- DMA through UHCI0 (one UART at a time), transparent: no SLIP escapes, no packet header. TX = one GDMA chain; RX
 * ends at line idle (IDLE_CONF threshold, uart_idle_eof_en) or when the buffer is full. The UART's own RX interrupt is
 * off while DMA owns the port. P4: the UHCI selects its UART in CONF0 b4:2 (7 = none). */
#define UHCI_CONF0   (HPPERIPH1 + 0x1F000 + 0x00)
#define UHCI_CONF1   (HPPERIPH1 + 0x1F000 + 0x14)
#define UHCI_ESCAPE  (HPPERIPH1 + 0x1F000 + 0x20)
static int uh_port = -1, uh_ch = -1;
static gdma_desc_t uh_txd[8], uh_rxd[8];

int uart_dma_start(int p, uint32_t idle_bits)
{
    if (p < 0 || p >= NPORT || uh_port >= 0) return -1;
    if (uh_ch < 0 && (uh_ch = gdma_alloc()) < 0) return -1;
    periph_enable(PERIPH_UHCI0);
    gdma_connect(uh_ch, GDMA_UHCI0);
    REG(UHCI_CONF0) = BIT(11);                                   /* clk_en (IDF uhci_ll_init) */
    REG(UHCI_CONF0) = BIT(11) | (uint32_t)p << 2 | BIT(8);       /* this UART, idle eof */
    REG(UHCI_CONF1) = 0;
    REG(UHCI_ESCAPE) = 0;
    REG_FIELD(IDLE_CONF(p), 0, 10, idle_bits ? idle_bits : 20);
    update(p);
    REG_CLR(INT_ENA(p), I_RXFULL | I_RXTOUT | I_RXOVF);
    uh_port = p;
    return 0;
}

void uart_dma_stop(int p)
{
    if (p != uh_port) return;
    gdma_reset(uh_ch);
    REG(UHCI_CONF0) = BIT(11) | 7u << 2;
    REG(INT_CLR(p)) = ~0u;
    REG_SET(INT_ENA(p), I_RXFULL | I_RXTOUT | I_RXOVF);
    uh_port = -1;
}

int uart_dma_write(int p, const void *buf, size_t n, uint32_t timeout_us)
{
    if (gdma_is_psram(buf)) return -1;                          /* TX DMA from PSRAM: refused (gdma.h) */
    if (p != uh_port || gdma_chain(uh_txd, 8, (void *)buf, n, true) < 0) return -1;
    gdma_out_reset(uh_ch);
    gdma_out_start(uh_ch, uh_txd);
    if (!timeout_us) return (int)n;                             /* async: poll uart_dma_write_done() */
    uint32_t t0 = ccount(), lim = timeout_us * (sys_cpu_hz() / 1000000);
    while (!gdma_out_eof(uh_ch)) if (ccount() - t0 > lim) return -1;
    return (int)n;                                              /* in the UART FIFO; uart_flush waits for the wire */
}

bool uart_dma_write_done(int p) { return p == uh_port && gdma_out_eof(uh_ch); }

int uart_dma_read_start(int p, void *buf, size_t max)
{
    if (p != uh_port || gdma_chain(uh_rxd, 8, buf, max, false) < 0) return -1;
    gdma_in_reset(uh_ch);
    gdma_in_start(uh_ch, uh_rxd);
    return 0;
}

int uart_dma_read_done(int p)                         /* bytes once the frame ended (idle / full), -1 before */
{
    if (p != uh_port) return -1;
    cache_dma_wbinv(uh_rxd, sizeof uh_rxd);           /* the DMA wrote the descriptors (and the buffer) behind the CPU's cache */
    if (!gdma_in_eof(uh_ch)) {                        /* the buffer filling up also ends it */
        gdma_desc_t *d = uh_rxd; while (d->next) d = d->next;
        if (d->owner) return -1;
    }
    int n = 0;
    for (gdma_desc_t *d = uh_rxd; d && !d->owner; d = d->next) { n += d->length; cache_dma_wbinv(d->buf, d->size); }
    return n;
}
