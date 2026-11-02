/* Host fuzzer of the GATT client (components/ble_host/gattc.c): the link is stubbed; whenever the client waits, the "server" answers
 * with random bytes (often a plausible opcode first). Nothing may crash, overrun a caller buffer, or wait for ever.
 * usage: fuzz_gattc [rounds] [seed] */
#include "gatt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long x = 1; static uint64_t now_us; static int connected = 1;
static unsigned rnd(void) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return (unsigned)x; }
int ble_hw_connected(void) { return connected; }
int ble_hw_selected(void) { return 0; }
int ble_hw_link_up(int h) { return h == 0 && connected; }
uint64_t time_us(void) { return now_us; }
int gatt_send_att(const uint8_t *a, unsigned n) { return (a && n <= 23) ? 0 : -1; }
unsigned att_mtu(void) { return 23; }
void gatt_poll(uint32_t wait_us)
{
    static const uint8_t ops[] = { 0x01, 0x03, 0x05, 0x09, 0x0B, 0x0D, 0x11, 0x13, 0x1B, 0x1D, 0x07, 0x0F, 0x17 };
    now_us += 2000 + wait_us;
    if (rnd() % 4 == 0) return;                                              /* silence: exercises the timeout */
    uint8_t p[40]; unsigned n = rnd() % 30;
    for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)rnd();
    if (n && (rnd() & 3)) p[0] = ops[rnd() % sizeof ops];
    if (n > 1 && (rnd() & 1)) p[1] = (uint8_t)(rnd() % 3 ? (rnd() & 1 ? 6 : 7) : rnd());     /* entry length byte */
    gattc_rx(p, n);
}
static unsigned cb_n;
static void cb(uint16_t h, const uint8_t *v, unsigned n) { cb_n++; volatile uint8_t s = 0; for (unsigned i = 0; i < n; i++) s ^= v[i]; (void)h; (void)s; }

int main(int argc, char **argv)
{
    unsigned long rounds = argc > 1 ? strtoul(argv[1], 0, 0) : 20000; x = argc > 2 ? strtoul(argv[2], 0, 0) | 1 : 7;
    for (unsigned long i = 0; i < rounds; i++) {
        gattc_svc_t s[4]; gattc_chr_t c[4]; unsigned n = 0; uint8_t buf[24]; uint8_t guard[8];
        memset(guard, 0xA5, sizeof guard);
        connected = (rnd() % 50) != 0;
        gattc_discover(s, 4, &n);
        gattc_svc_t sv = { 1, (uint16_t)(rnd() % 40 + 1), 0xFFF0 };
        gattc_chars(&sv, c, 4, &n);
        gattc_read((uint16_t)rnd(), buf, sizeof buf);
        gattc_write((uint16_t)rnd(), buf, rnd() % 25, rnd() & 1);
        gattc_chr_t ch = { 5, 6, (uint16_t)(rnd() & 1 ? 7 : 0), 0x10, 0xFFF2 };
        gattc_subscribe(&ch, rnd() & 1, cb);
        for (unsigned j = 0; j < sizeof guard; j++) if (guard[j] != 0xA5) { puts("FAIL stack guard"); return 1; }
    }
    printf("fuzz_gattc: %lu rounds, %u notifications dispatched, no crash\n", rounds, cb_n);
    return 0;
}
