#include "usb.h"
#include "console.h"
#include "sys.h"
#include "cpu.h"

int main(void)
{
    usb_dev_function(USB_FN_CDC);
    int r = usb_dev_start();
    printf("usb_dev_start: %d\n", r);
    uint32_t t0 = ccount();
    bool ready = false;
    while (ccount() - t0 < sys_cpu_hz() * 10u) { usb_dev_poll(); if (usb_cdc_ready()) { ready = true; break; } }
    usb_stats_t st; usb_get_stats(&st);
    printf("configured by a host: %s | bus resets %lu, setup packets %lu, stalls %lu\n", ready ? "yes" : "no (no cable?)",
           (unsigned long)st.resets, (unsigned long)st.setups, (unsigned long)st.stalls);
    if (ready) {   /* echo whatever the host sends for 3 s */
        const char *hello = "esp-ndf P4 usb_p4 hello\r\n";
        while (!usb_cdc_write(hello, 25)) usb_dev_poll();
        t0 = ccount();
        uint8_t b[64]; size_t total = 0;
        while (ccount() - t0 < sys_cpu_hz() * 3u) { usb_dev_poll(); size_t n = usb_cdc_read(b, sizeof b); if (n) { total += n; usb_cdc_write(b, n); } }
        printf("echoed %lu bytes\n", (unsigned long)total);
    }
    usb_dev_stop();
    printf("usb_p4 done\n");
    return 0;
}
