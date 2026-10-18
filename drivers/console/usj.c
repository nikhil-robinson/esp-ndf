/* USB-Serial-JTAG console: 64-byte IN FIFO, flushed with WR_DONE. */
#include "console.h"
#include "soc.h"
#include "xtensa.h"
#include "sys.h"

#define FIFO_FREE() (REG(USJ_EP1_CONF) & BIT(1))
#define TIMEOUT_US  2000          /* host not reading: drop instead of hanging */

void console_init(void) {}

int console_try_write(const char *buf, int n)
{
    if (NDF_CONSOLE == NDF_CONSOLE_NONE) return n;
    int k = 0;
    while (k < n && FIFO_FREE()) REG(USJ_EP1) = (uint8_t)buf[k++];
    REG(USJ_EP1_CONF) = BIT(0);
    return k;
}

int console_write(const char *buf, int n)
{
    if (NDF_CONSOLE == NDF_CONSOLE_NONE) return n;
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
