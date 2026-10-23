/* USB OTG full-speed CDC-ACM device test (usb.h). The console stays on USB Serial/JTAG; "otg <s>" hands the PHY to
 * our OTG device for s seconds (the USB-JTAG port leaves the bus), then hands it back and prints the counters.
 * On the CDC port: 'S' + u32 count -> device sends count bytes (byte k = k & 0xFF); 'U' + u32 count -> device
 * receives count bytes, checks the same pattern, replies u32 received + u32 errors; other bytes are echoed.
 * Host side: tools/usb_cdc_test.py. */
#include "console.h"
#include "sys.h"
#include "usb.h"
#include <stdlib.h>
#include <string.h>

static char line[64];

static void otg_run(uint32_t secs)
{
    printf("otg: PHY to OTG for %lu s\n", (unsigned long)secs); delay_ms(50);   /* let the line reach the host */
    int rc = usb_dev_start();
    static uint8_t buf[8128] __attribute__((aligned(4)));
    for (uint32_t k = 0; k < sizeof buf; k++) buf[k] = (uint8_t)k;   /* pattern source, made once */
    uint32_t t0 = time_ms(), mode = 0, left = 0, pos = 0, got = 0, err = 0, have = 0; uint8_t hdr[5];
    while (rc == 0 && time_ms() - t0 < secs * 1000) {
        usb_dev_poll();
        if (!usb_cdc_ready()) continue;
        if (mode == 1) {                                     /* streaming to the host */
            if (!left) { mode = 0; continue; }
            uint32_t n = left < 7936 ? left : 7936;      /* 31 x 256: the pattern stays continuous across chunks */
            if (usb_cdc_write(buf, n)) { pos += n; left -= n; }
            continue;
        }
        static uint8_t in[512]; size_t n = usb_cdc_read(in, sizeof in);
        for (size_t k = 0; k < n; k++) {
            if (mode == 2) {                                 /* receiving from the host */
                if (in[k] != (uint8_t)got) err++;
                if (++got == left) {
                    uint32_t r[2] = { got, err }; mode = 0;
                    while (!usb_cdc_write(r, 8)) usb_dev_poll();
                }
                continue;
            }
            if (have || in[k] == 'S' || in[k] == 'U') {      /* command header: 'S'/'U' + u32 */
                hdr[have++] = in[k];
                if (have == 5) {
                    uint32_t c; memcpy(&c, hdr + 1, 4); have = 0;
                    if (hdr[0] == 'S') { mode = 1; left = c; pos = 0; } else { mode = 2; left = c; got = err = 0; }
                }
                continue;
            }
            while (!usb_cdc_write(&in[k], 1)) usb_dev_poll();   /* echo */
        }
    }
    usb_stats_t s; usb_get_stats(&s);
    usb_dev_stop();
    delay_ms(1500);                                          /* USB Serial/JTAG re-enumerates on the host */
    printf("otg rc %d resets %lu setups %lu stalls %lu rx %lu tx %lu xfers %lu\n", rc, (unsigned long)s.resets,
           (unsigned long)s.setups, (unsigned long)s.stalls, (unsigned long)s.rx_bytes, (unsigned long)s.tx_bytes,
           (unsigned long)s.tx_xfers);
}

int main(void)
{
    delay_ms(300);
    printf("usb_cdc ready\n");
    int k = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0 || ch == '\r') continue;
        if (ch != '\n') { if (k < (int)sizeof line - 1) line[k++] = (char)ch; continue; }
        line[k] = 0; k = 0;
        if (!strncmp(line, "otg ", 4)) otg_run((uint32_t)atoi(line + 4));
        printf("ok\n");
    }
}
