/* BLE GATT server on the S3's own link layer (components/ble_host): service 0xFFF0 with
 *   0xFFF1  read / write, 16 bytes          0xFFF2  read / notify, a 32-bit counter (10 notifications per second)
 * next to GAP (device name) and GATT. Address 11:22:33:44:55:C6 (static random), name "NDF-GATT". */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "ble_hw.h"
#include "gatt.h"
#include "rf_pll.h"
#include "rf_init.h"
#include <string.h>

static uint8_t val[16];
static uint32_t counter;
static volatile uint64_t burst_until;                            /* writing 0xB5 to 0xFFF1 starts 5 s of notifications as fast as the link takes them */
static int rd_val(uint8_t *o, uint16_t max) { (void)max; memcpy(o, val, sizeof val); return sizeof val; }
static int wr_val(const uint8_t *in, uint16_t n) { if (n && in[0] == 0xB5) burst_until = time_us() + 5000000ull; memset(val, 0, sizeof val); memcpy(val, in, n > sizeof val ? sizeof val : n); return 0; }
static int rd_cnt(uint8_t *o, uint16_t max) { (void)max; memcpy(o, &counter, 4); return 4; }
static gatt_chr_t chrs[2] = { { 0xFFF1, GATT_P_READ | GATT_P_WRITE, rd_val, wr_val, 0 }, { 0xFFF2, GATT_P_READ | GATT_P_NOTIFY, rd_cnt, 0, 0 } };

int main(void)
{
    delay_ms(1000);
    printf("=== ble_gatts\n");
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2402000, &r);
    printf("ble_hw_init rc %d\n", ble_hw_init());
    static const uint8_t addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };
    gatts_init("NDF-GATT");
    printf("service rc %d, handles value %u counter %u\n", gatts_add_service(0xFFF0, chrs, 2), chrs[0].handle, chrs[1].handle);
    printf("advertise rc %d\n", gatts_advertise(addr, 20000));
    uint32_t next = 0, sec = 0, sent = 0, fail = 0;
    for (;;) {
        gatt_poll(burst_until > time_us() ? 1000 : 20000);
        if (burst_until > time_us() && (gatts_subscribed(&chrs[1]) & 1))
            for (int k = 0; k < 8; k++) { uint32_t c = counter + 1; if (gatts_notify(&chrs[1], (uint8_t *)&c, 4)) break; counter = c; sent++; }
        uint32_t now = time_us() / 100000;                      /* 10 Hz */
        if (now != next) {
            next = now;
            if (burst_until <= time_us() && (gatts_subscribed(&chrs[1]) & 1)) { uint32_t c = counter + 1; if (gatts_notify(&chrs[1], (uint8_t *)&c, 4)) fail++; else { counter = c; sent++; } }
            if (now / 10 != sec) { sec = now / 10; { ble_hw_stats_t st; ble_hw_get_stats(&st); uint8_t cs; uint16_t iv = 0; ble_hw_conn_info(&cs, &iv); printf("t %lu s: %s, notified %lu, not sent %lu, events %lu, disconnects %lu, interval %u x 1.25 ms\n", (unsigned long)sec, ble_hw_connected() ? "connected" : "advertising", (unsigned long)sent, (unsigned long)fail, (unsigned long)st.events, (unsigned long)st.disconnects, iv); } }
        }
    }
}
