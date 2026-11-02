/* BLE central on the S3's own link layer: initiates a connection to the IDF peer (public address 24:58:7C:E2:C9:4A unless NDF_PEER_ADDR is given as a C initializer, e.g.
 * -DNDF_PEER_ADDR='{0x4A,0xC9,0xE2,0x7C,0x58,0x24}'), runs the GATT client of ble_gattc over the link (discovery, read, write, notifications) and prints "gattc PASS" / "gattc FAIL ...".
 * After the run the link is dropped and the whole thing repeats. */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "ble_hw.h"
#include "gatt.h"
#include "rf_pll.h"
#include "rf_init.h"
#include <string.h>
#include "client.h"
#ifdef NDF_PEER_IS_NDF                                  /* -DNDF_PEER_IS_NDF: the peer is an esp-ndf ble_gatts server (static random address 11:22:33:44:55:C6) */
#define NDF_PEER_RANDOM 1
#define NDF_PEER_ADDR { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 }
#endif
#ifndef NDF_PEER_RANDOM
#define NDF_PEER_RANDOM 0
#endif
#ifndef NDF_PEER_ADDR
#define NDF_PEER_ADDR { 0x4A, 0xC9, 0xE2, 0x7C, 0x58, 0x24 }
#endif

int main(void)
{
    delay_ms(1000);
    printf("=== ble_central\n");
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2402000, &r);
    printf("ble_hw_init rc %d\n", ble_hw_init());
    gatts_init("NDF-CEN");
    static const uint8_t peer[6] = NDF_PEER_ADDR;
    for (;;) {
        printf("initiating rc %d\n", gattc_connect(peer, NDF_PEER_RANDOM, 15000));
        { ble_hw_stats_t st; ble_hw_get_stats(&st); printf("stats events %lu skips %lu packets %lu isr %lu\n", (unsigned long)st.events, (unsigned long)st.skips, (unsigned long)st.packets, (unsigned long)st.isr_calls); }
        { extern uint32_t ble_hw_idbg[8]; printf("idbg txd %04x %04x cs0 %04x hits %lu | events %lu pkts %lu last rxstat/hdr %08x cs1c/20 %08x\n", (unsigned)ble_hw_idbg[0], (unsigned)ble_hw_idbg[1], (unsigned)ble_hw_idbg[2], (unsigned long)ble_hw_idbg[3], (unsigned long)ble_hw_idbg[4], (unsigned long)ble_hw_idbg[5], (unsigned)ble_hw_idbg[7], (unsigned)ble_hw_idbg[6]); }
        if (!ble_hw_connected()) { printf("central: not connected\n"); continue; }
        printf("central: connected, master %d\n", ble_hw_is_master());
        uint64_t up = time_us();
        while (ble_hw_connected() && time_us() - up < 1500000) gatt_poll(20000);
        if (ble_hw_connected()) run_client();
        ble_hw_disconnect();
        delay_ms(1500);
    }
}
