/* BLE central on the S3's own link layer: initiates a connection to the IDF peer (public address 24:58:7C:E2:C9:4A unless NDF_PEER_ADDR is given as a C initializer, e.g.
 * -DNDF_PEER_ADDR='{0x4A,0xC9,0xE2,0x7C,0x58,0x24}'), runs the GATT client of ble_gattc over the link (discovery, read, write, notifications) and prints "gattc PASS" / "gattc FAIL ...".
 * After the run the link is dropped and the whole thing repeats. */
#include "console.h"
#include "sys.h"
#include "cpu.h"
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
        if (!ble_hw_connected()) { printf("central: not connected\n"); continue; }
        printf("central: connected, master %d\n", ble_hw_is_master());
        uint64_t up = time_us();
        while (ble_hw_connected() && time_us() - up < 1500000) gatt_poll(20000);
        if (ble_hw_connected()) run_client();
        ble_hw_disconnect();
        delay_ms(1500);
    }
}
