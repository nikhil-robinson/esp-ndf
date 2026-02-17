/*
 * ESP32-S3 Debug Output Implementation
 * USB Serial/JTAG and UART backends
 */

#include "debug.h"
#include "../reg/usb_serial_jtag_reg.h"
#include "../reg/uart_reg.h"
#include "../arch/esp32s3/esp32s3.h"

/* Current debug backend */
static debug_backend_t current_backend = DEBUG_USB_SERIAL;

/* Hex characters */
static const char hex_chars[] = "0123456789ABCDEF";

/*
 * Simple USB Serial/JTAG putchar - writes single byte at a time
 * This matches how the ROM bootloader outputs text
 */
static void usb_serial_putc(char c)
{
    /* Wait for EP1 buffer to be free (ready for write) */
    uint32_t timeout = 500000;
    while (!(REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG) & USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE)) {
        if (--timeout == 0) {
            return;  /* USB not connected or host not reading */
        }
    }
    
    /* Write byte to EP1 FIFO */
    REG_WRITE(USB_SERIAL_JTAG_EP1_REG, c);
    
    /* Signal write done - send the byte */
    REG_SET_BIT(USB_SERIAL_JTAG_EP1_CONF_REG, USB_SERIAL_JTAG_WR_DONE);
}

static int usb_serial_available(void)
{
    return (REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG) & USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL) ? 1 : 0;
}

static char usb_serial_getc(void)
{
    /* Wait for data */
    while (!usb_serial_available()) {
        /* Busy wait */
    }
    
    return REG_READ(USB_SERIAL_JTAG_OUT_EP1_REG) & 0xFF;
}

static void usb_serial_flush(void)
{
    /* Wait for TX complete */
    uint32_t timeout = 100000;
    while (!(REG_READ(USB_SERIAL_JTAG_EP1_CONF_REG) & USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE)) {
        if (--timeout == 0) return;
    }
}

/*
 * UART output
 */
static void uart_putc_internal(uint32_t uart_num, char c)
{
    /* Wait for TX FIFO space */
    while (((REG_READ(UART_STATUS_REG(uart_num)) >> UART_TXFIFO_CNT_S) & 0x3FF) >= 128) {
        /* Wait */
    }
    REG_WRITE(UART_FIFO_REG(uart_num), c);
}

static int uart_available_internal(uint32_t uart_num)
{
    return ((REG_READ(UART_STATUS_REG(uart_num)) >> UART_RXFIFO_CNT_S) & 0x3FF) > 0;
}

static char uart_getc_internal(uint32_t uart_num)
{
    while (!uart_available_internal(uart_num)) {
        /* Wait */
    }
    return REG_READ(UART_FIFO_REG(uart_num)) & 0xFF;
}

static void uart_flush_internal(uint32_t uart_num)
{
    while (((REG_READ(UART_STATUS_REG(uart_num)) >> UART_TXFIFO_CNT_S) & 0x3FF) > 0) {
        /* Wait */
    }
}

/*
 * Public API
 */
void debug_init(debug_backend_t backend)
{
    current_backend = backend;
    
    if (backend == DEBUG_USB_SERIAL) {
        /* Enable USB memory clock */
        #define USB_SERIAL_JTAG_MEM_CONF_REG    (DR_REG_USB_SERIAL_JTAG_BASE + 0x48)
        #define USB_SERIAL_JTAG_USB_MEM_CLK_EN  (1 << 1)
        REG_SET_BIT(USB_SERIAL_JTAG_MEM_CONF_REG, USB_SERIAL_JTAG_USB_MEM_CLK_EN);
        
        /* Enable USB pad and D+ pullup (needed for enumeration) */
        REG_SET_BIT(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
        REG_SET_BIT(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
        
        /* Wait for USB to enumerate (host to connect) */
        /* Give time for USB enumeration - ~500ms */
        for (volatile int i = 0; i < 2000000; i++) {
            __asm__ volatile("nop");
        }
    } else {
        /* Initialize UART */
        uint32_t uart_num = (backend == DEBUG_UART0) ? 0 : 1;
        
        /* For UART0: TX=GPIO43, RX=GPIO44 (default strapping) */
        /* For UART1: User must configure GPIOs separately */
        if (backend == DEBUG_UART0) {
            /* GPIO43 (U0TXD) - configured by ROM bootloader already */
            /* GPIO44 (U0RXD) - configured by ROM bootloader already */
            /* The ROM bootloader sets up UART0 for boot messages */
            /* We just need to reconfigure the baud rate */
        }
        
        /* Configure 115200 8N1 */
        uint32_t clk_div = UART_CLK_FREQ / 115200;
        REG_WRITE(UART_CLKDIV_REG(uart_num), clk_div);
        
        /* 8 data bits, 1 stop bit, no parity */
        uint32_t conf0 = (UART_DATA_8_BITS << UART_BIT_NUM_S) | 
                         (UART_STOP_1_BIT << UART_STOP_BIT_NUM_S);
        REG_WRITE(UART_CONF0_REG(uart_num), conf0);
        
        /* Reset FIFOs */
        REG_SET_BIT(UART_CONF0_REG(uart_num), UART_RXFIFO_RST | UART_TXFIFO_RST);
        REG_CLR_BIT(UART_CONF0_REG(uart_num), UART_RXFIFO_RST | UART_TXFIFO_RST);
    }
}

void debug_putc(char c)
{
    /* Convert \n to \r\n for terminal compatibility */
    if (c == '\n') {
        debug_putc('\r');
    }
    
    switch (current_backend) {
        case DEBUG_USB_SERIAL:
            usb_serial_putc(c);
            break;
        case DEBUG_UART0:
            uart_putc_internal(0, c);
            break;
        case DEBUG_UART1:
            uart_putc_internal(1, c);
            break;
    }
}

void debug_puts(const char *str)
{
    while (*str) {
        debug_putc(*str++);
    }
}

void debug_println(const char *str)
{
    debug_puts(str);
    debug_putc('\n');
}

void debug_hex8(uint8_t val)
{
    debug_putc(hex_chars[(val >> 4) & 0x0F]);
    debug_putc(hex_chars[val & 0x0F]);
}

void debug_hex32(uint32_t val)
{
    debug_puts("0x");
    for (int i = 7; i >= 0; i--) {
        debug_putc(hex_chars[(val >> (i * 4)) & 0x0F]);
    }
}

void debug_hexdump(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    
    for (uint32_t i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            debug_hex32(i);
            debug_puts(": ");
        }
        
        debug_hex8(p[i]);
        debug_putc(' ');
        
        if ((i % 16) == 15 || i == len - 1) {
            debug_putc('\n');
        }
    }
}

/* Simple printf implementation */
void debug_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (s) debug_puts(s);
                    else debug_puts("(null)");
                    break;
                }
                case 'd':
                case 'i': {
                    int val = va_arg(args, int);
                    if (val < 0) {
                        debug_putc('-');
                        val = -val;
                    }
                    /* Convert to string (simple, max 10 digits) */
                    char buf[12];
                    int i = 0;
                    if (val == 0) {
                        buf[i++] = '0';
                    } else {
                        while (val > 0) {
                            buf[i++] = '0' + (val % 10);
                            val /= 10;
                        }
                    }
                    /* Print in reverse */
                    while (i > 0) {
                        debug_putc(buf[--i]);
                    }
                    break;
                }
                case 'u': {
                    unsigned int val = va_arg(args, unsigned int);
                    char buf[12];
                    int i = 0;
                    if (val == 0) {
                        buf[i++] = '0';
                    } else {
                        while (val > 0) {
                            buf[i++] = '0' + (val % 10);
                            val /= 10;
                        }
                    }
                    while (i > 0) {
                        debug_putc(buf[--i]);
                    }
                    break;
                }
                case 'x':
                case 'X': {
                    unsigned int val = va_arg(args, unsigned int);
                    debug_puts("0x");
                    /* Print hex */
                    int started = 0;
                    for (int i = 7; i >= 0; i--) {
                        int nibble = (val >> (i * 4)) & 0x0F;
                        if (nibble != 0 || started || i == 0) {
                            debug_putc(hex_chars[nibble]);
                            started = 1;
                        }
                    }
                    break;
                }
                case 'p': {
                    void *ptr = va_arg(args, void *);
                    debug_hex32((uint32_t)ptr);
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    debug_putc(c);
                    break;
                }
                case '%':
                    debug_putc('%');
                    break;
                default:
                    debug_putc('%');
                    debug_putc(*fmt);
                    break;
            }
        } else {
            debug_putc(*fmt);
        }
        fmt++;
    }
    
    va_end(args);
}

int debug_available(void)
{
    switch (current_backend) {
        case DEBUG_USB_SERIAL:
            return usb_serial_available();
        case DEBUG_UART0:
            return uart_available_internal(0);
        case DEBUG_UART1:
            return uart_available_internal(1);
    }
    return 0;
}

char debug_getc(void)
{
    switch (current_backend) {
        case DEBUG_USB_SERIAL:
            return usb_serial_getc();
        case DEBUG_UART0:
            return uart_getc_internal(0);
        case DEBUG_UART1:
            return uart_getc_internal(1);
    }
    return 0;
}

void debug_flush(void)
{
    switch (current_backend) {
        case DEBUG_USB_SERIAL:
            usb_serial_flush();
            break;
        case DEBUG_UART0:
            uart_flush_internal(0);
            break;
        case DEBUG_UART1:
            uart_flush_internal(1);
            break;
    }
}
