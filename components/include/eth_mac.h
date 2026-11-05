/* Ethernet MAC (ESP32-P4 only, TRM chapter 55): 10/100 Mbit/s MAC with its DMA, RMII to an external PHY, MDIO/MDC for the PHY
 * registers, raw frame send / receive (polled; a netif glue is the network stack's). Default pins (the P4 function-board
 * wiring): RMII TXD0 GPIO34, TXD1 35, TX_EN 49, RXD0 29, RXD1 30, CRS_DV 28, REF_CLK in GPIO50 (or 32), MDC GPIO31, MDIO GPIO52;
 * all but MDC / MDIO are fixed IO MUX pins (function 3). The 50 MHz reference comes from the PHY (REF_CLK input). */
#pragma once
#include "ndf.h"

typedef struct {
    uint8_t mac[6];
    int phy_addr;                 /* 0..31, -1: scan */
    int mdc_pin, mdio_pin;        /* GPIO matrix routed (default 31 / 52 when 0) */
    int refclk_pin;               /* 32, 44 or 50 (default 50 when 0) */
    int phy_reset_pin;            /* -1: none */
} eth_cfg_t;
typedef struct { uint16_t id1, id2; bool link; int speed; bool full; } eth_phy_t;   /* speed 10 / 100 */

int  eth_init(const eth_cfg_t *c);                           /* MAC + DMA ring set-up, PHY reset and autonegotiation started */
int  eth_phy_read(int reg, uint16_t *v);
int  eth_phy_write(int reg, uint16_t v);
int  eth_link_update(eth_phy_t *p);                          /* reads the PHY, programs speed / duplex into the MAC when the link is up */
int  eth_send(const void *frame, size_t len);               /* 14..1518 bytes (no FCS); waits for a free descriptor */
int  eth_recv(void *buf, size_t cap);                        /* a received frame length (no FCS), 0 if none, -1 on error */
void eth_promiscuous(bool on);
uint32_t eth_dropped(void);                                  /* frames dropped for lack of descriptors / buffers */
