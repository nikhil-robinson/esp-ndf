/* Hardware BLE scan, no vendor code: our PHY init, then the S3's own link-layer core receives
 * advertising PDUs on channel 37 by itself (demodulation, access address, CRC, dewhitening).
 * Prints the first PDUs, then per second: PDUs received and the CPU share of the interrupt. */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "ble_hw.h"
#ifndef SHOW
#define SHOW 3
#endif
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"

int main(void)
{
    delay_ms(1000);
    printf("=== ble_scan (hardware link layer, no vendor code)\n");
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2402000, &r);
    rf_rx_on();
    uint32_t t0 = ccount();
    int rc = ble_hw_init();
    printf("ble_hw_init rc %d, %lu us\n", rc, (unsigned long)((ccount() - t0) / 240));
    ble_hw_scan(37);
    ble_hw_stats_t s0 = { 0 }, s;
    int shown = 0;
    for (int sec = 0; sec < 5; sec++) {
        uint32_t ts = ccount();
        uint32_t idle = 0;
        while (ccount() - ts < 240000000u) {
            ble_hw_pkt_t p;
            uint32_t w = ccount();
            int got = ble_hw_wait(&p, 1000);
            idle += ccount() - w;
            if (got && shown++ < SHOW) {
                printf("  pdu type %d len %d ch %d rssi %d:", p.hdr0 & 0xF, p.len, p.chan, p.rssi);
                for (int k = 0; k < p.len && k < 37; k++) printf(" %02x", p.data[k]);
                printf("\n");
            }
        }
        ble_hw_get_stats(&s);
        printf("  (time inside ble_hw_wait, asleep or servicing: %.1f%%)\n", idle / 2.4e6);
        printf("t %d s: %lu PDUs/s, %lu events/s, service CPU %.2f%%, dropped %lu, watchdog re-arms %lu, interrupts %lu\n", sec, (unsigned long)(s.packets - s0.packets),
               (unsigned long)(s.events - s0.events), (double)(s.isr_cycles - s0.isr_cycles) / 2.4e6, (unsigned long)s.dropped, (unsigned long)s.aborts, (unsigned long)s.isr_calls);
        s0 = s;
    }
    ble_hw_stop();
    printf("=== done\n");
    for (;;) {}
}
