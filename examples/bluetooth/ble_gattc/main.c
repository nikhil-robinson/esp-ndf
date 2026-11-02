/* BLE GATT client on the S3's own link layer: advertises, is connected by a central, then (1.5 s later) uses that same connection as a
 * client toward the central's GATT server: discovers its services and characteristics, reads the device name and 0xFFF1, writes
 * 0xFFF1 and reads it back, subscribes to 0xFFF2 and counts notifications. Prints "gattc PASS" or "gattc FAIL <why>". */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "ble_hw.h"
#include "gatt.h"
#include "rf_pll.h"
#include "rf_init.h"
#include <string.h>

#include "client.h"

int main(void)
{
    delay_ms(1000);
    printf("=== ble_gattc\n");
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2402000, &r);
    printf("ble_hw_init rc %d\n", ble_hw_init());
    static const uint8_t addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };
    gatts_init("NDF-GATTC");
    printf("advertise rc %d\n", gatts_advertise(addr, 20000));
    uint64_t up = 0; int done = 0;
    for (;;) {
        gatt_poll(20000);
        if (!ble_hw_connected()) { up = 0; done = 0; continue; }
        if (!up) up = time_us();
        if (!done && time_us() - up > 1500000) { done = 1; run_client(); }
    }
}
