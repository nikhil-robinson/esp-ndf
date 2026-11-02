/* Advertising-channel sniffer on the S3's own link layer: hops over 37 / 38 / 39 and prints every CONNECT_IND (type 5) and SCAN_REQ (type 3) with all its bytes, and counts the
 * ADV_IND per channel. A third board's view of what a central puts on the air. */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "ble_hw.h"
#include "rf_pll.h"
#include "rf_init.h"

int main(void)
{
    delay_ms(1000);
    printf("=== ble_sniff\n");
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2402000, &r);
    printf("ble_hw_init rc %d\n", ble_hw_init());
    unsigned adv[3] = { 0, 0, 0 }, ci = 0, k = 0;
#ifdef FIXED_CH
    rf_pll_set_khz(2402000, &r); ble_hw_scan(37);                  /* one continuous scan on channel 37 */
#endif
    for (;;) {
        static const uint16_t mhz[3] = { 2402, 2426, 2480 };
#ifdef FIXED_CH
        const unsigned c = 0;
#else
        const unsigned c = k % 3;
#endif
#ifndef FIXED_CH
        rf_pll_set_khz(mhz[c] * 1000, &r);
        ble_hw_scan((uint8_t)(37 + c));
#endif
        uint64_t t0 = time_us();
        while (time_us() - t0 < 60000) {
            ble_hw_pkt_t p;
            if (!ble_hw_wait(&p, 2000)) continue;
            unsigned ty = p.hdr0 & 0xF;
            if (ty == 0) adv[c]++;
            if (ty == 5 || ty == 3) {
                if (ty == 5) ci++;
                printf("PDU type %u len %u ch %u hdr %02x rssi %d:", ty, p.len, 37 + c, p.hdr0, p.rssi);
                for (unsigned j = 0; j < p.len && j < 40; j++) printf(" %02x", p.data[j]);
                printf("\n");
            }
        }
        if (++k % 150 == 0) printf("adv 37 %u 38 %u 39 %u, connect_ind %u\n", adv[0], adv[1], adv[2], ci);
    }
}
