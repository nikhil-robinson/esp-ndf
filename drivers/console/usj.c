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

/* Host present = USB start-of-frame packets arrive (one per ms): the frame number moves. No frame for 10 ms
 * means unplugged (or the host suspended the port): writes are dropped at once instead of waiting 2 ms each. */
#define USJ_FRAM_NUM 0x60038024u
static uint32_t sof_last, sof_t;
bool console_host_present(void)
{
    if (NDF_CONSOLE != NDF_CONSOLE_USJ) return true;
    uint32_t f = REG(USJ_FRAM_NUM) & 0x7FF, now = ccount();
    if (f != sof_last) { sof_last = f; sof_t = now; return true; }
    return now - sof_t < 10000u * (sys_cpu_hz() / 1000000);
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
    if (!console_host_present()) return n;                 /* unplugged: drop without waiting */
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

/* One received byte, or -1 if none (never waits).
 * USJ: EP1_CONF bit 2 = OUT data available, read EP1. UART0: STATUS[9:0] = RX count. */
int console_getc(void)
{
    if (NDF_CONSOLE == NDF_CONSOLE_UART0) return (REG(0x6000001C) & 0x3FF) ? (int)(REG(U0_FIFO) & 0xFF) : -1;
    if (NDF_CONSOLE == NDF_CONSOLE_USJ) return (REG(USJ_EP1_CONF) & BIT(2)) ? (int)(REG(USJ_EP1) & 0xFF) : -1;
    return -1;
}
