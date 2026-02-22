/* =============================================================================
 * uart.c  –  ESP32-S3 UART driver implementation
 * ============================================================================= */
#include "drivers/uart/uart.h"
#include "drivers/gpio/gpio.h"
#include "soc/soc.h"
#include "soc/system_reg.h"

/* ---- GPIO signal numbers for UART peripheral connections in GPIO matrix -- */
/*
 * These are the "signal output" numbers used in GPIO_FUNC_OUT_SEL_CFG_REG.
 * From ESP32-S3 TRM, Table 6-3/6-4 (GPIO Matrix signal list).
 */
#define U0TXD_OUT_IDX    15   /* UART0 TXD output signal */
#define U0RXD_IN_IDX     14   /* UART0 RXD input  signal */
#define U1TXD_OUT_IDX    17   /* UART1 TXD output signal */
#define U1RXD_IN_IDX     16   /* UART1 RXD input  signal */
#define U2TXD_OUT_IDX    19   /* UART2 TXD output signal */
#define U2RXD_IN_IDX     18   /* UART2 RXD input  signal */

/* Default GPIO assignments (customise as needed) */
static const uint8_t uart_txd_gpio[3] = {43, 17, 19};
static const uint8_t uart_rxd_gpio[3] = {44, 18, 20};

static const uint8_t uart_txd_sig[3] = {U0TXD_OUT_IDX, U1TXD_OUT_IDX, U2TXD_OUT_IDX};
static const uint8_t uart_rxd_sig[3] = {U0RXD_IN_IDX,  U1RXD_IN_IDX,  U2RXD_IN_IDX};

/* ============================================================================
 * Private helpers
 * ============================================================================ */

/**
 * Connect UART TXD/RXD signals to GPIO pins via the GPIO matrix.
 *  TXD: GPIO_FUNC_OUT_SEL_CFG tells which peripheral signal drives this GPIO.
 *  RXD: GPIO_FUNC_IN_SEL_CFG  tells which GPIO drives this peripheral input.
 */
static void uart_gpio_matrix_connect(int port)
{
    uint8_t txd_gpio = uart_txd_gpio[port];
    uint8_t rxd_gpio = uart_rxd_gpio[port];

    /* TXD: set IO_MUX to GPIO mode, output enable */
    gpio_set_direction(txd_gpio, GPIO_MODE_OUTPUT);

    /* Route UART TXD signal to the TXD GPIO via the output matrix */
    REG_WRITE(GPIO_FUNC_OUT_SEL_CFG_REG(txd_gpio),
              (uart_txd_sig[port] << GPIO_OUT_SEL_S));

    /* RXD: set IO_MUX to GPIO mode, input enable with pull-up */
    gpio_set_direction(rxd_gpio, GPIO_MODE_INPUT_PULLUP);

    /* Route RXD GPIO into the UART RXD input signal */
    REG_WRITE(GPIO_FUNC_IN_SEL_CFG_REG(uart_rxd_sig[port]),
              (rxd_gpio << GPIO_IN_SEL_S) | (1u << GPIO_SIG_IN_SEL_S));
}

/**
 * Calculate and apply the baud rate divisor.
 *
 * UART clock source: APB (80 MHz)
 * Baud rate = UART_CLK / (clk_div_int + clk_div_frag / 16)
 * where UART_CLK = APB_CLK_FREQ / APB_PRE_DIV (usually 80 MHz)
 *
 * UART_CLKDIV_REG[27:4] = integer part
 * UART_CLKDIV_REG[3:0]  = fractional part (0..15, units = 1/16)
 */
static void uart_set_baud(int port, uint32_t baud)
{
    uint32_t clk_src_hz = APB_CLK_FREQ_HZ;  /* 80 MHz */
    uint32_t div_int    = clk_src_hz / baud;
    uint32_t div_frac   = ((clk_src_hz * 16) / baud) - div_int * 16;

    /* Write UART_CLKDIV_REG */
    uint32_t clkdiv_val = (div_int  << 4) | (div_frac & 0xF);
    REG_WRITE(UART_CLKDIV(port), clkdiv_val);

    /* Set CLK_CONF: enable clock, select APB source */
    uint32_t clk_conf = REG_READ(UART_CLK_CONF(port));
    clk_conf &= ~(0x3u << UART_CLK_SEL_S);
    clk_conf |=  (UART_CLK_APB << UART_CLK_SEL_S) | (1u << UART_CLK_CONF_EN_S);
    REG_WRITE(UART_CLK_CONF(port), clk_conf);
}

/* ============================================================================
 * Public API implementation
 * ============================================================================ */

int uart_init(int port, const uart_config_t *cfg)
{
    if (port < 0 || port > 2) return -1;
    if (!cfg) return -1;

    /* ---- 1. Enable peripheral clock and release from reset -------------- */
    uint32_t clk_mask = (port == 0) ? SYSTEM_UART_CLK_EN  :
                        (port == 1) ? SYSTEM_UART1_CLK_EN :
                                      SYSTEM_UART_MEM_CLK_EN;
    (void)clk_mask;   /* clocks enabled in board_init; skip re-enabling here */

    /* ---- 2. Reset TX and RX FIFOs --------------------------------------- */
    uint32_t conf0 = REG_READ(UART_CONF0(port));
    conf0 |=  (1u << UART_TXFIFO_RST_S) | (1u << UART_RXFIFO_RST_S);
    REG_WRITE(UART_CONF0(port), conf0);
    conf0 &= ~((1u << UART_TXFIFO_RST_S) | (1u << UART_RXFIFO_RST_S));
    REG_WRITE(UART_CONF0(port), conf0);

    /* ---- 3. Configure frame format (CONF0) ------------------------------ */
    conf0 = 0;

    /* Data bits */
    conf0 |= ((cfg->data_bits & 0x3) << UART_BIT_NUM_S);

    /* Stop bits */
    conf0 |= ((cfg->stop_bits & 0x3) << UART_STOP_BIT_NUM_S);

    /* Parity */
    if (cfg->parity != UART_NO_PARITY) {
        conf0 |= (1u << UART_PARITY_EN_S);
        if (cfg->parity == UART_ODD_PARITY) {
            conf0 |= (1u << UART_PARITY_S);
        }
    }

    /* Enable memory clock, TX/RX */
    conf0 |= (1u << UART_MEM_CLK_EN_S);

    REG_WRITE(UART_CONF0(port), conf0);

    /* ---- 4. Baud rate --------------------------------------------------- */
    uart_set_baud(port, cfg->baud_rate);

    /* ---- 5. CONF1: interrupt thresholds --------------------------------- */
    /* RX full at 120 bytes, TX empty at 10 bytes, RX timeout at 10 symbols */
    REG_WRITE(UART_CONF1(port),
              (120u << 0)  |  /* rxfifo_full_thrd  */
              (10u  << 8)  |  /* txfifo_empty_thrd */
              (10u  << 24)    /* rx_tout_thrhd     */
    );

    /* ---- 6. Connect TXD/RXD to GPIO via GPIO matrix -------------------- */
    uart_gpio_matrix_connect(port);

    return 0;
}


void uart_putc(int port, uint8_t c)
{
    /* Wait until there is space in the TX FIFO */
    while (((REG_READ(UART_STATUS(port)) & UART_TXFIFO_CNT_M)
             >> UART_TXFIFO_CNT_S) >= UART_FIFO_LEN) {
        /* spin */
    }
    REG_WRITE(UART_FIFO(port), (uint32_t)c);
}


uint8_t uart_getc(int port)
{
    /* Wait until there is data in the RX FIFO */
    while (((REG_READ(UART_STATUS(port)) & UART_RXFIFO_CNT_M)
             >> UART_RXFIFO_CNT_S) == 0) {
        /* spin */
    }
    return (uint8_t)(REG_READ(UART_FIFO(port)) & 0xFF);
}


void uart_puts(int port, const char *s)
{
    while (*s) {
        uart_putc(port, (uint8_t)*s++);
    }
}


void uart_read(int port, uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = uart_getc(port);
    }
}


int uart_rx_available(int port)
{
    return (int)((REG_READ(UART_STATUS(port)) & UART_RXFIFO_CNT_M)
                  >> UART_RXFIFO_CNT_S);
}


int uart_tx_idle(int port)
{
    uint32_t status = REG_READ(UART_STATUS(port));
    uint32_t txcnt  = (status & UART_TXFIFO_CNT_M) >> UART_TXFIFO_CNT_S;
    /* TX is idle when FIFO is empty and no transmission is in progress */
    return (txcnt == 0) ? 1 : 0;
}


void uart_flush_tx(int port)
{
    while (!uart_tx_idle(port)) { /* spin */ }
}


/* ============================================================================
 * uart_printf – minimal formatted output (no floating point)
 * ============================================================================ */

static void _uart_put_uint(int port, uint32_t v, int base,
                           int width, char pad, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char  buf[32];
    int   i = 0;

    if (v == 0) {
        buf[i++] = '0';
    } else {
        while (v > 0) {
            buf[i++] = digits[v % (uint32_t)base];
            v /= (uint32_t)base;
        }
    }

    /* Pad to width */
    while (i < width) {
        buf[i++] = pad;
    }

    /* Reverse and send */
    for (int j = i - 1; j >= 0; j--) {
        uart_putc(port, (uint8_t)buf[j]);
    }
}

int uart_printf(int port, const char *fmt, ...)
{
    /* Manual va_list: Xtensa windowed ABI passes extra args on stack */
    typedef struct { const char *fmt; } va_hack;
    /* Use GCC's built-in va_list */
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    int count = 0;

    while (*fmt) {
        if (*fmt != '%') {
            uart_putc(port, (uint8_t)*fmt++);
            count++;
            continue;
        }
        fmt++;   /* skip '%' */

        /* Parse optional flags */
        char  pad   = ' ';
        int   width = 0;

        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            int32_t v = __builtin_va_arg(ap, int32_t);
            if (v < 0) { uart_putc(port, '-'); v = -v; count++; }
            _uart_put_uint(port, (uint32_t)v, 10, width, pad, false);
            break;
        }
        case 'u':
            _uart_put_uint(port, __builtin_va_arg(ap, uint32_t), 10, width, pad, false);
            break;
        case 'x':
            _uart_put_uint(port, __builtin_va_arg(ap, uint32_t), 16, width, pad, false);
            break;
        case 'X':
            _uart_put_uint(port, __builtin_va_arg(ap, uint32_t), 16, width, pad, true);
            break;
        case 'p':
            uart_puts(port, "0x");
            _uart_put_uint(port, __builtin_va_arg(ap, uint32_t), 16, 8, '0', false);
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (s) { uart_puts(port, s); } else { uart_puts(port, "(null)"); }
            break;
        }
        case 'c':
            uart_putc(port, (uint8_t)__builtin_va_arg(ap, int));
            count++;
            break;
        case '%':
            uart_putc(port, '%');
            count++;
            break;
        default:
            uart_putc(port, '%');
            uart_putc(port, (uint8_t)*fmt);
            count += 2;
            break;
        }
        fmt++;
    }

    __builtin_va_end(ap);
    return count;
}
