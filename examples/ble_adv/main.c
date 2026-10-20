/* Hardware BLE advertising, no vendor code: the S3's link-layer core sends ADV_NONCONN_IND
 * (CRC, whitening, GFSK by the hardware) on channel 37 every 20 ms; the CPU only schedules
 * the next event from the end-of-event interrupt. */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "ble_hw.h"
#include "../../drivers/rf/rf_pll.h"
#include "../../drivers/rf/rf_init.h"

int main(void)
{
    delay_ms(1000);
    printf("=== ble_adv (hardware link layer, no vendor code)\n");
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2402000, &r);
    // rf_rx_on(): forces the front end to receive; the core drives TX/RX itself
    printf("ble_hw_init rc %d\n", ble_hw_init());
    static const uint8_t addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };   /* random static, LSB first */
    static const uint8_t data[] = { 2, 1, 6, 7, 9, 'N', 'D', 'F', '-', 'H', 'W' };
    delay_ms(100);
    printf("ble_hw_adv rc %d\n", ble_hw_adv(37, addr, 1, data, sizeof data, 20000));
    ble_hw_stats_t s0 = { 0 }, s;
    for (int sec = 0; sec < 10; sec++) {
        uint32_t t = ccount(); while (ccount() - t < 240000000u) { ble_hw_pkt_t p; ble_hw_wait(&p, 100000); }
        ble_hw_get_stats(&s);
        printf("t %d s: %lu adverts/s, CPU %.3f%%, skips %lu\n", sec, (unsigned long)(s.events - s0.events),
               (double)(s.isr_cycles - s0.isr_cycles) / 2.4e6, (unsigned long)s.skips);
        s0 = s;
    }
    printf("=== done\n");
    for (;;) {}
}
