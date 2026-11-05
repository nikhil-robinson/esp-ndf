/* P4 Ethernet MAC (eth_mac.h). Flow of ESP-IDF v6.1 esp_hal_emac/emac_hal.c, esp32p4/emac_ll.h, esp_eth/src/mac/esp_eth_mac_esp*.c: EMAC bus
 * clock, reset in LP_CLKRST, RMII with the reference clock from the pad (HP_SYSTEM GMAC_CTRL0 phy_intf_sel = 4), RX / TX /
 * TXRX pad clocks, MAC defaults (96-bit IFG, carrier sense, 100M full duplex until the PHY says otherwise, broadcast passed),
 * DMA defaults (enhanced 32-byte descriptors, 16 beat bursts, TX threshold 64, RX threshold 64, flush on no descriptor),
 * chained descriptor rings, MDIO through GMIIADDR / GMIIDATA with the CSR clock range for the AHB frequency. The PHY side is the
 * generic IEEE 802.3 register set (BMCR / BMSR / ANAR / ANLPAR). NOT VERIFIED ON HARDWARE. */
#include "eth_mac.h"
#include "gpio.h"
#include "clk.h"
#include "cpu.h"
#include "spin.h"
#include "soc.h"
#include "cache_p4.h"
#include "regs/emac.h"
#include "regs/hp_clkrst.h"
#include "regs/lp_clkrst.h"
#include "regs/p4reg.h"
#include <string.h>

#define M(r)  EMAC_##r##_REG
#define RXN   8
#define TXN   4
#define BUFSZ 1600
#define HP_SYS_GMAC_CTRL0 (HP_SYS_BASE + 0x14C)
#define MDC_SIG   108
#define MDO_SIG   109
#define MDI_SIG   107
#define IOMUX_F3  (3u << 12)

typedef struct edesc { uint32_t d0, d1, d2, d3, ex[4]; } edesc_t;             /* enhanced descriptor, 8 words */
static edesc_t rxd[RXN] __attribute__((aligned(64))), txd[TXN] __attribute__((aligned(64)));
static uint8_t rxb[RXN][BUFSZ] __attribute__((aligned(64))), txb[TXN][BUFSZ] __attribute__((aligned(64)));
static struct { bool up; int phy, rxi, txi; uint32_t dropped; } e;

static void pin_mux(int pin) { REG(IO_MUX_BASE + 4 + pin * 4) = IOMUX_F3 | BIT(9) | 2u << 10; }          /* function 3, input enable, drive 2 */

static int mdio_wait(void) { SPIN_US(REG(M(GMIIADDRESS)) & 1, 1000); return REG(M(GMIIADDRESS)) & 1 ? -1 : 0; }
int eth_phy_read(int reg, uint16_t *v)
{
    if (mdio_wait()) return -1;
    REG(M(GMIIADDRESS)) = (uint32_t)e.phy << 11 | (uint32_t)reg << 6 | /* CSR clock range 100..150 MHz: 0b0100 */ 4u << 2 | 1;
    if (mdio_wait()) return -1;
    *v = (uint16_t)REG(M(GMIIDATA));
    return 0;
}
int eth_phy_write(int reg, uint16_t v)
{
    if (mdio_wait()) return -1;
    REG(M(GMIIDATA)) = v;
    REG(M(GMIIADDRESS)) = (uint32_t)e.phy << 11 | (uint32_t)reg << 6 | 4u << 2 | BIT(1) | 1;
    return mdio_wait();
}

int eth_init(const eth_cfg_t *c)
{
    if (!c || (c->mac[0] & 1)) return -1;
    int mdc = c->mdc_pin ? c->mdc_pin : 31, mdio = c->mdio_pin ? c->mdio_pin : 52, ref = c->refclk_pin ? c->refclk_pin : 50;
    if (ref != 32 && ref != 44 && ref != 50) return -1;
    /* pads */
    static const uint8_t rmii[6] = { 34, 35, 49, 29, 30, 28 };
    for (int i = 0; i < 6; i++) pin_mux(rmii[i]);
    pin_mux(ref);
    gpio_config(mdc, GPIO_OUTPUT);  gpio_out_signal(mdc, MDC_SIG, false);
    gpio_config(mdio, GPIO_INPUT | GPIO_OUTPUT | GPIO_PULLUP); gpio_out_signal(mdio, MDO_SIG, false); gpio_in_signal(mdio, MDI_SIG, false);
    if (c->phy_reset_pin >= 0) { gpio_config(c->phy_reset_pin, GPIO_OUTPUT); gpio_write(c->phy_reset_pin, false); for (volatile int i = 0; i < 400000; i++) {} gpio_write(c->phy_reset_pin, true); for (volatile int i = 0; i < 2000000; i++) {} }
    /* clocks */
    uint32_t t = irq_lock();
    FSET(HP_SYS_CLKRST_REG_EMAC_SYS_CLK_EN);
    FSET(LP_CLKRST_RST_EN_EMAC); FCLR(LP_CLKRST_RST_EN_EMAC);
    REG_FIELD(HP_SYS_GMAC_CTRL0, 2, 3, 4);                                      /* RMII */
    FCLR(HP_SYS_CLKRST_REG_PAD_EMAC_REF_CLK_EN);                                /* the PHY drives REF_CLK */
    FSET(HP_SYS_CLKRST_REG_EMAC_RMII_CLK_EN); FPUT(HP_SYS_CLKRST_REG_EMAC_RMII_CLK_SRC_SEL, 0);
    FSET(HP_SYS_CLKRST_REG_EMAC_RX_CLK_EN); FPUT(HP_SYS_CLKRST_REG_EMAC_RX_CLK_SRC_SEL, 0); FPUT(HP_SYS_CLKRST_REG_EMAC_RX_CLK_DIV_NUM, 1);
    FSET(HP_SYS_CLKRST_REG_EMAC_TX_CLK_EN); FPUT(HP_SYS_CLKRST_REG_EMAC_TX_CLK_SRC_SEL, 0); FPUT(HP_SYS_CLKRST_REG_EMAC_TX_CLK_DIV_NUM, 1);
    FCLR(LP_CLKRST_HP_PAD_EMAC_TX_CLK_EN); FCLR(LP_CLKRST_HP_PAD_EMAC_RX_CLK_EN); FSET(LP_CLKRST_HP_PAD_EMAC_TXRX_CLK_EN);
    irq_unlock(t);
    /* DMA software reset */
    REG(M(BUSMODE)) = BIT(0);
    SPIN_US(REG(M(BUSMODE)) & 1, 100000);
    if (REG(M(BUSMODE)) & 1) return -1;                                         /* no reference clock: reset never completes */
    /* MAC defaults */
    REG(M(INTERRUPTMASK)) = 0xFFFFFFFFu;
    REG(M(MACCONFIGURATION)) = BIT(23) | BIT(22) | BIT(15) | BIT(14) | BIT(11) | BIT(10) | 0u << 17;   /* WD, JD, PS(MII/RMII), FES 100M, DM full, IPC checksum offload, IFG 96 */
    REG(M(MACFRAMEFILTER)) = 0;                                                 /* DA filter normal mode, broadcast passed */
    REG(M(MACADDRESS0HIGH)) = BIT(31) | (uint32_t)c->mac[5] << 8 | c->mac[4];
    REG(M(MACADDRESS0LOW)) = (uint32_t)c->mac[3] << 24 | (uint32_t)c->mac[2] << 16 | (uint32_t)c->mac[1] << 8 | c->mac[0];
    /* DMA: enhanced descriptors (ATDS), PBL 16, mixed burst, aligned beats; chained rings */
    REG(M(BUSMODE)) = BIT(7) | 16u << 8 | BIT(26) | BIT(25);
    for (int i = 0; i < RXN; i++) {
        rxd[i].d0 = BIT(31);                                                    /* DMA owns */
        rxd[i].d1 = BIT(14) | BUFSZ;                                            /* chained, buffer 1 size */
        rxd[i].d2 = (uint32_t)(uintptr_t)rxb[i];
        rxd[i].d3 = (uint32_t)(uintptr_t)&rxd[(i + 1) % RXN];
    }
    for (int i = 0; i < TXN; i++) {
        txd[i].d0 = BIT(20);                                                    /* chained, CPU owns */
        txd[i].d2 = (uint32_t)(uintptr_t)txb[i];
        txd[i].d3 = (uint32_t)(uintptr_t)&txd[(i + 1) % TXN];
    }
    cache_dma_writeback(rxd, sizeof rxd); cache_dma_writeback(txd, sizeof txd);
    REG(M(RECEIVEDESCRIPTORLISTADDRESS)) = (uint32_t)(uintptr_t)rxd;
    REG(M(TRANSMITDESCRIPTORLISTADDRESS)) = (uint32_t)(uintptr_t)txd;
    /* OPERATIONMODE: DT = 0 (drop checksum-error frames), DFF = 0 (flush frames when no descriptor), RSF / TSF off, thresholds 64 (RTC / TTC = 0),
     * OSF on (second frame); then flush the transmit FIFO (FTF self-clears) */
    REG(M(OPERATIONMODE)) = BIT(2);
    REG_SET(M(OPERATIONMODE), BIT(20));
    SPIN_US(REG(M(OPERATIONMODE)) & BIT(20), 10000);
    REG(M(STATUS)) = 0xFFFFFFFFu;
    e.phy = c->phy_addr;
    if (e.phy < 0) {                                                            /* scan for a PHY that answers */
        for (int a = 0; a < 32; a++) { uint16_t v; e.phy = a; if (!eth_phy_read(2, &v) && v != 0xFFFF && v != 0) break; e.phy = -1; }
        if (e.phy < 0) return -1;
    }
    eth_phy_write(0, BIT(15));                                                  /* BMCR reset */
    SPIN_US(({ uint16_t v = BIT(15); eth_phy_read(0, &v); v & BIT(15); }), 500000);
    eth_phy_write(4, 0x01E1);                                                   /* advertise 10/100 half / full + 802.3 selector */
    eth_phy_write(0, BIT(12) | BIT(9));                                         /* autonegotiation enable + restart */
    REG_SET(M(MACCONFIGURATION), BIT(3) | BIT(2));                              /* TE, RE */
    REG_SET(M(OPERATIONMODE), BIT(13) | BIT(1));                                /* start transmit / receive DMA */
    e.rxi = e.txi = 0; e.up = true;
    return 0;
}

int eth_link_update(eth_phy_t *p)
{
    if (!e.up) return -1;
    uint16_t bmsr, anar, anlp, id1 = 0, id2 = 0;
    eth_phy_read(1, &bmsr); eth_phy_read(1, &bmsr);                             /* link status latches low */
    eth_phy_read(2, &id1); eth_phy_read(3, &id2);
    bool link = bmsr & BIT(2), done = bmsr & BIT(5);
    int speed = 10; bool full = false;
    if (link && done) {
        eth_phy_read(4, &anar); eth_phy_read(5, &anlp);
        uint16_t common = anar & anlp;
        if (common & (BIT(8) | BIT(7))) speed = 100; else speed = 10;
        full = common & (BIT(8) | BIT(6));
        REG_FIELD(M(MACCONFIGURATION), 14, 1, speed == 100);                    /* FES */
        REG_FIELD(M(MACCONFIGURATION), 11, 1, full);                            /* DM */
    }
    if (p) { p->id1 = id1; p->id2 = id2; p->link = link && done; p->speed = speed; p->full = full; }
    return 0;
}

int eth_send(const void *frame, size_t len)
{
    if (!e.up || !frame || len < 14 || len > 1518) return -1;
    edesc_t *d = &txd[e.txi];
    cache_dma_wbinv(d, sizeof *d);
    SPIN_US(d->d0 & BIT(31), 100000);
    if (d->d0 & BIT(31)) return -1;
    memcpy(txb[e.txi], frame, len);
    cache_dma_writeback(txb[e.txi], len);
    d->d1 = (uint32_t)len;
    d->d0 = BIT(31) | BIT(29) | BIT(28) | BIT(20) | 3u << 22;                   /* OWN, LS, FS, chained, checksum insertion full */
    cache_dma_writeback(d, sizeof *d);
    REG(M(TRANSMITPOLLDEMAND)) = 1;
    e.txi = (e.txi + 1) % TXN;
    return 0;
}

int eth_recv(void *buf, size_t cap)
{
    if (!e.up) return -1;
    edesc_t *d = &rxd[e.rxi];
    cache_dma_wbinv(d, sizeof *d);
    if (d->d0 & BIT(31)) {
        if (REG(M(STATUS)) & BIT(7)) { REG(M(STATUS)) = BIT(7) | BIT(16); REG(M(RECEIVEPOLLDEMAND)) = 1; }       /* RU: restart reception */
        return 0;
    }
    uint32_t d0 = d->d0;
    int len = 0;
    bool ok = (d0 & BIT(9)) && (d0 & BIT(8)) && !(d0 & BIT(15));              /* first + last in one descriptor, no error summary */
    if (ok) {
        len = (int)((d0 >> 16) & 0x3FFF) - 4;                                   /* frame length includes the FCS */
        cache_dma_wbinv(rxb[e.rxi], BUFSZ);
        if (len < 0 || (size_t)len > cap) { len = -1; e.dropped++; }
        else memcpy(buf, rxb[e.rxi], len);
    } else { len = -1; e.dropped++; }
    d->d0 = BIT(31);                                                            /* give the descriptor back */
    cache_dma_writeback(d, sizeof *d);
    REG(M(RECEIVEPOLLDEMAND)) = 1;
    e.rxi = (e.rxi + 1) % RXN;
    return len;
}

void eth_promiscuous(bool on) { REG_FIELD(M(MACFRAMEFILTER), 0, 1, on); }
uint32_t eth_dropped(void) { return e.dropped; }
