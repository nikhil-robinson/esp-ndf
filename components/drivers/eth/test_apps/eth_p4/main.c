#include "eth_mac.h"
#include "console.h"
#include "sys.h"
#include <string.h>

int main(void)
{
    int err = 0;
    eth_cfg_t c = { .mac = { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 }, .phy_addr = -1, .phy_reset_pin = -1 };
    err += eth_init(&c) != 0;
    eth_phy_t p = { 0 };
    for (int i = 0; i < 100 && !p.link; i++) { eth_link_update(&p); for (volatile int k = 0; k < 4000000; k++) {} }
    printf("phy id %04x:%04x link %d %d Mbit/s %s\n", p.id1, p.id2, p.link, p.speed, p.full ? "full" : "half");
    err += !p.link;
    uint8_t arp[42] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0, 0, 0x12, 0x34, 0x56, 0x08, 0x06, 0, 1, 8, 0, 6, 4, 0, 1,
                        0x02, 0, 0, 0x12, 0x34, 0x56, 192, 168, 1, 50, 0, 0, 0, 0, 0, 0, 192, 168, 1, 1 };
    err += eth_send(arp, sizeof arp) != 0;
    uint8_t rx[1600]; int got = 0;
    for (int i = 0; i < 4000000 && !got; i++) { int n = eth_recv(rx, sizeof rx); if (n > 0) got = n; }
    printf("received %d bytes\n", got);
    printf("eth test errors %d\n", err);
    return err != 0;
}
