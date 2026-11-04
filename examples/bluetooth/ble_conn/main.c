/* BLE peripheral on the S3's own link layer, no vendor code: connectable advertising, the connection
 * (CONNECT_IND, CSA#1 hopping, LL control by ble_hw_ll_control) and a small GATT server:
 *   1 service 0x1800 (GAP)    2 char decl (read, 0x2A00)      3 Device Name "NDF-CONN"
 *   4 service 0xFFE0          5 char decl (read|notify, 0xFFE1) 6 value: 32-bit counter   7 CCCD
 * The counter is notified once a second while the central has enabled notifications. */
#include "console.h"
#include "sys.h"
#include "xtensa.h"
#include "ble_hw.h"
#include <string.h>
#include "rf_pll.h"
#include "rf_init.h"

static uint32_t counter;
static uint16_t cccd;
static const char name[] = "NDF-CONN";

static void att_send(const uint8_t *a, unsigned n)     /* one ATT PDU in an L2CAP frame (CID 4) */
{
    uint8_t r[27] = { (uint8_t)n, 0, 4, 0 };
    memcpy(r + 4, a, n);
    ble_hw_send(2, r, 4 + n);
}
static void att_err(uint8_t op, uint16_t h, uint8_t e) { uint8_t a[5] = { 0x01, op, (uint8_t)h, (uint8_t)(h >> 8), e }; att_send(a, 5); }

static void att(const uint8_t *a, unsigned n)
{
    uint16_t s = n >= 3 ? a[1] | a[2] << 8 : 0, e = n >= 5 ? a[3] | a[4] << 8 : 0, uuid = n >= 7 ? a[5] | a[6] << 8 : 0;
    uint8_t r[23];
    switch (a[0]) {
    case 0x02: r[0] = 0x03; r[1] = 23; r[2] = 0; att_send(r, 3); break;                     /* MTU */
    case 0x10:                                                                              /* Read By Group Type */
        if (uuid != 0x2800) { att_err(0x10, s, 0x10); break; }
        if (s <= 1) { uint8_t g[] = { 0x11, 6, 1, 0, 3, 0, 0x00, 0x18, 4, 0, 7, 0, 0xE0, 0xFF }; att_send(g, sizeof g); }
        else if (s <= 4) { uint8_t g[] = { 0x11, 6, 4, 0, 7, 0, 0xE0, 0xFF }; att_send(g, sizeof g); }
        else att_err(0x10, s, 0x0A);
        break;
    case 0x08:                                                                              /* Read By Type */
        if (uuid == 0x2803) {
            if (s <= 2) { uint8_t g[] = { 0x09, 7, 2, 0, 0x02, 3, 0, 0x00, 0x2A }; att_send(g, sizeof g); }
            else if (s <= 5) { uint8_t g[] = { 0x09, 7, 5, 0, 0x12, 6, 0, 0xE1, 0xFF }; att_send(g, sizeof g); }
            else att_err(0x08, s, 0x0A);
        } else if (uuid == 0x2A00 && s <= 3) { r[0] = 0x09; r[1] = 2 + sizeof name - 1; r[2] = 3; r[3] = 0; memcpy(r + 4, name, sizeof name - 1); att_send(r, 4 + sizeof name - 1); }
        else att_err(0x08, s, 0x0A);
        break;
    case 0x04:                                                                              /* Find Information */
        if (s <= 7 && e >= 7) { uint8_t g[] = { 0x05, 1, 7, 0, 0x02, 0x29 }; att_send(g, sizeof g); }
        else att_err(0x04, s, 0x0A);
        break;
    case 0x0A:                                                                              /* Read */
        if (s == 3) { r[0] = 0x0B; memcpy(r + 1, name, sizeof name - 1); att_send(r, sizeof name); }
        else if (s == 6) { r[0] = 0x0B; memcpy(r + 1, &counter, 4); att_send(r, 5); }
        else if (s == 7) { r[0] = 0x0B; r[1] = cccd; r[2] = cccd >> 8; att_send(r, 3); }
        else att_err(0x0A, s, 0x01);
        break;
    case 0x12:                                                                              /* Write */
        if (s == 7 && n >= 5) { cccd = a[3] | a[4] << 8; r[0] = 0x13; att_send(r, 1); printf("  CCCD = %u\n", cccd); }
        else att_err(0x12, s, 0x03);
        break;
    default: att_err(a[0], 0, 0x06);
    }
    printf("  ATT 0x%02x start %u uuid %04x\n", a[0], s, uuid);
}

static void l2cap(const uint8_t *d, unsigned len)
{
    if (len < 5) return;
    unsigned cid = d[2] | d[3] << 8;
    if (cid == 4) att(d + 4, len - 4);
    else if (cid == 5) {                         /* L2CAP signalling: command reject */
        uint8_t r[10] = { 6, 0, 5, 0, 0x01, d[5], 2, 0, 0, 0 };
        ble_hw_send(2, r, 10);
        printf("  L2CAP signalling code 0x%02x -> reject\n", d[4]);
    }
}

int main(void)
{
    delay_ms(1000);
    printf("=== ble_conn (hardware link layer, no vendor code)\n");
    rf_init_replay();
    rf_pll_result_t r; rf_pll_set_khz(2402000, &r);
    printf("ble_hw_init rc %d\n", ble_hw_init());
    static const uint8_t addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };
    static const uint8_t data[] = { 2, 1, 6, 9, 9, 'N', 'D', 'F', '-', 'C', 'O', 'N', 'N' };
    ble_hw_adv_type(0);
    printf("ble_hw_adv rc %d\n", ble_hw_adv(37, addr, 1, data, sizeof data, 20000));
    int was = 0; uint32_t t1 = 0;
    ble_hw_stats_t s0 = { 0 }, s;
    for (int sec = 0; sec < 120; ) {
        ble_hw_pkt_t p;
        int got = ble_hw_wait(&p, 50000), c = ble_hw_connected();
        if (got && c) {
            if ((p.hdr0 & 3) == 1 && !p.len) {}                 /* empty PDU */
            else if (ble_hw_ll_control(&p)) printf("  LL control 0x%02x\n", p.data[0]);
            else if ((p.hdr0 & 3) == 2) l2cap(p.data, p.len);
        }
        if (c != was) { printf(c ? "CONNECTED\n" : "DISCONNECTED\n"); was = c; if (!c) cccd = 0; }
        if (time_us() / 1000000 != t1) {
            t1 = time_us() / 1000000; sec++;
            if (c && (cccd & 1)) { counter++; uint8_t nt[7] = { 0x1B, 6, 0 }; memcpy(nt + 3, &counter, 4); att_send(nt, 7); }
            ble_hw_get_stats(&s);
            printf("t %d s: %s, events %lu/s, PDUs %lu/s, TX %lu, RX errors %lu, disconnects %lu, CPU %.3f%%\n", sec, c ? "connected" : "advertising", (unsigned long)(s.events - s0.events),
                   (unsigned long)(s.packets - s0.packets), (unsigned long)s.tx_pdus, (unsigned long)s.rx_errors, (unsigned long)s.disconnects, (double)(s.isr_cycles - s0.isr_cycles) / 2.4e6);
            s0 = s;
        }
    }
    printf("=== done\n");
    for (;;) {}
}
