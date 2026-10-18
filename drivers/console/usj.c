/* Console: USB-Serial-JTAG (64-byte IN FIFO, flushed with WR_DONE) or UART0
 * (128-byte FIFO written directly, polled: safe from ISRs). */
#include "console.h"
#include "soc.h"
#include "xtensa.h"
#include "sys.h"
#include "uart.h"

#define U0_FIFO     0x60000000
#define U0_TXCNT()  ((REG(0x6000001C) >> 16) & 0x3FF)

#define FIFO_FREE() (REG(USJ_EP1_CONF) & BIT(1))
#define TIMEOUT_US  2000          /* host not reading: drop instead of hanging */

void console_init(void)
{
    if (NDF_CONSOLE == NDF_CONSOLE_UART0) {
        uart_cfg_t c = { .baud = NDF_CONSOLE_BAUD, .tx_pin = NDF_CONSOLE_TX_PIN, .rx_pin = NDF_CONSOLE_RX_PIN };
        uart_init(0, &c);
    }
}

static int u0_write(const char *buf, int n, bool block)
{
    int k = 0;
    while (k < n) {
        if (U0_TXCNT() < 128) REG(U0_FIFO) = (uint8_t)buf[k++];
        else if (!block) break;
    }
    return k;
}

int console_try_write(const char *buf, int n)
{
    if (NDF_CONSOLE == NDF_CONSOLE_NONE) return n;
    if (NDF_CONSOLE == NDF_CONSOLE_UART0) return u0_write(buf, n, false);
    int k = 0;
    while (k < n && FIFO_FREE()) REG(USJ_EP1) = (uint8_t)buf[k++];
    REG(USJ_EP1_CONF) = BIT(0);
    return k;
}

int console_write(const char *buf, int n)
{
    if (NDF_CONSOLE == NDF_CONSOLE_NONE) return n;
    if (NDF_CONSOLE == NDF_CONSOLE_UART0) return u0_write(buf, n, true);
    uint32_t t = ccount(), lim = TIMEOUT_US * (sys_cpu_hz() / 1000000);
    int k = 0;
    while (k < n) {
        if (FIFO_FREE()) {
            REG(USJ_EP1) = (uint8_t)buf[k++];
            t = ccount();
        } else {
            REG(USJ_EP1_CONF) = BIT(0);
            if (ccount() - t > lim) break;
        }
    }
    REG(USJ_EP1_CONF) = BIT(0);
    return k;
}
