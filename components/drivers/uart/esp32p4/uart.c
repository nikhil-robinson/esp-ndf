/* P4 UART driver, same API as drivers/uart/uart.c. UART0-4 (ESP-IDF v6.1 hw_ver3 uart_reg.h): the UART core runs in
 * its own clock domain, so configuration writes take effect on UART_REG_UPDATE; FIFO 128 bytes, 8-bit counts.
 * Core clock = XTAL 40 MHz (HP_SYS_CLKRST PERI_CLK_CTRL110 + 4 n), independent of the CPU frequency.
 * Interrupt-driven RX and TX rings as on the S3. ponytail: RS485 / IrDA / pattern / flow control / DMA return -1
 * here until they are ported (the S3 driver has them). */
#include "uart.h"
#include "spin.h"
#include "gpio.h"
#include "clk.h"
#include "irq.h"
#include "cpu.h"
#include "sys.h"
#include "soc.h"

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

#define RING NDF_UART_RING
typedef struct {
    uint8_t rx[RING], tx[RING];
    volatile uint32_t rh, rt, th, tt;
    volatile uint32_t ovf;
    bool irq_on;
} port_t;
static port_t up[NPORT];

static const periph_t pid[NPORT] = { PERIPH_UART0, PERIPH_UART1, PERIPH_UART2, PERIPH_UART2, PERIPH_UART2 };

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
    if (st & (I_RXFULL | I_RXTOUT | I_RXOVF)) {
        uint32_t n = REG(STATUS(p)) & 0xFF;
        while (n--) {
            uint8_t b = REG(FIFO(p));
            if (u->rh - u->rt < RING) u->rx[u->rh++ & (RING - 1)] = b;
            else u->ovf++;
        }
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

    if (p <= 1) periph_enable(pid[p]);      /* ponytail: the gate table has UART0/1; UART2-4 rely on their reset state */
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
    u->rh = u->rt = u->th = u->tt = 0; u->ovf = 0;
    if (c->tx_pin >= 0) {
        gpio_config(c->tx_pin, GPIO_OUTPUT | GPIO_INPUT | (p == 0 ? GPIO_FORCE : 0));
        gpio_write(c->tx_pin, 1);
        gpio_out_signal(c->tx_pin, 10 + 3 * p, false);       /* UARTn_TXD_PAD_OUT_IDX */
    }
    if (c->rx_pin >= 0) {
        gpio_config(c->rx_pin, GPIO_INPUT | GPIO_PULLUP | (p == 0 ? GPIO_FORCE : 0));
        gpio_in_signal(c->rx_pin, 10 + 3 * p, false);        /* UARTn_RXD_PAD_IN_IDX */
    }
    REG(INT_CLR(p)) = ~0u;
    if (!u->irq_on) u->irq_on = irq_attach(SRC_UART0 + p, 1, uart_isr, (void *)p) >= 0;
    REG(INT_ENA(p)) = I_RXFULL | I_RXTOUT | I_RXOVF;
    return (uint32_t)((uint64_t)SCLK_HZ * 16 / div16);
}

int uart_write(int p, const void *buf, size_t n)
{
    if (p < 0 || p >= NPORT) return -1;
    port_t *u = &up[p];
    const uint8_t *b = buf;
    size_t i = 0;
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

int uart_set_flow(int p, int rts_pin, int cts_pin, uint32_t rx_thresh) { (void)p; (void)rts_pin; (void)cts_pin; (void)rx_thresh; return -1; }
int uart_set_mode(int p, int mode, int de_pin) { (void)de_pin; return p >= 0 && p < NPORT && mode == UART_MODE_UART ? 0 : -1; }
uint32_t uart_rs485_clashes(int p) { (void)p; return 0; }
int uart_set_pattern(int p, char c, int n, uint32_t gap, uint32_t pre, uint32_t post) { (void)p; (void)c; (void)n; (void)gap; (void)pre; (void)post; return -1; }
int uart_pattern_pop(int p) { (void)p; return -1; }
int  uart_dma_start(int p, uint32_t idle_bits) { (void)p; (void)idle_bits; return -1; }
void uart_dma_stop(int p) { (void)p; }
int  uart_dma_write(int p, const void *buf, size_t n, uint32_t timeout_us) { (void)p; (void)buf; (void)n; (void)timeout_us; return -1; }
bool uart_dma_write_done(int p) { (void)p; return true; }
int  uart_dma_read_start(int p, void *buf, size_t max) { (void)p; (void)buf; (void)max; return -1; }
int  uart_dma_read_done(int p) { (void)p; return -1; }
