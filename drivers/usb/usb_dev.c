/* USB OTG full-speed device, CDC-ACM (usb.h). DWC2 core in buffer-DMA mode (the core moves every packet between RAM
 * and its FIFOs; the CPU only programs transfers), polled.
 * Registers per TRM ch. 32 (DWC2 OTG_FS); PHY switch per ch. 10 (RTC_CNTL_USB_CONF) and ch. 33.
 * Endpoints: EP0 control, EP1 IN bulk (data to host), EP2 OUT bulk (data from host), EP3 IN interrupt (CDC
 * notifications; required by the class, never used). FIFO RAM 256 words: RX, EP0 TX, EP1 TX, EP3 TX, DMA endpoint info (see RXF_W).
 * DMA buffers are in internal SRAM, 4-byte aligned (no data cache there: nothing to flush). */
#include "usb.h"
#include "soc.h"
#include "clk.h"
#include "gpio.h"
#include "sys.h"
#include "xtensa.h"
#include <string.h>
#include "wdt.h"

/* OTG guard: while the PHY belongs to OTG the USB Serial/JTAG link is gone, so a hang or a halted panic would leave
 * the board unreachable. The RTC watchdog (RTC reset: clears RTC_CNTL_USB_CONF, the PHY select - proven by
 * examples/usb_safety_proof) runs for the whole OTG session and is fed from usb_dev_poll(). */
#ifndef NDF_USB_GUARD_MS
#define NDF_USB_GUARD_MS 3000
#endif
static uint32_t guard_fed;

#define OTG             USB_OTG_BASE
#define R(o)            REG(OTG + (o))
#define GOTGCTL         0x000
#define GAHBCFG         0x008
#define GUSBCFG         0x00C
#define GRSTCTL         0x010
#define GINTSTS         0x014
#define GINTMSK         0x018
#define GRXFSIZ         0x024
#define GNPTXFSIZ       0x028
#define GDFIFOCFG       0x05C                   /* DMA mode: endpoint-info base in FIFO RAM [31:16], FIFO depth [15:0] */
#define DIEPTXF(n)      (0x104 + 4 * ((n) - 1))
#define DCFG            0x800
#define DCTL            0x804
#define DIEPMSK         0x810
#define DOEPMSK         0x814
#define DAINT           0x818
#define DAINTMSK        0x81C
#define DIEPCTL(n)      (0x900 + 0x20 * (n))
#define DIEPINT(n)      (0x908 + 0x20 * (n))
#define DIEPTSIZ(n)     (0x910 + 0x20 * (n))
#define DOEPCTL(n)      (0xB00 + 0x20 * (n))
#define DOEPINT(n)      (0xB08 + 0x20 * (n))
#define DOEPTSIZ(n)     (0xB10 + 0x20 * (n))
#define DIEPDMA(n)      (0x914 + 0x20 * (n))
#define DOEPDMA(n)      (0xB14 + 0x20 * (n))
#define PCGCCTL         0xE00

#define GINT_USBRST     BIT(12)
#define GINT_ENUMDNE    BIT(13)
#define GINT_IEPINT     BIT(18)
#define GINT_OEPINT     BIT(19)
#define EP_ENA          BIT(31)
#define EP_SNAK         BIT(27)
#define EP_CNAK         BIT(26)
#define EP_STALL        BIT(21)
#define EP_SD0PID       BIT(28)
#define EP_ACTIVE       BIT(15)
#define INT_XFRC        BIT(0)
#define INT_STUP        BIT(3)
#define INT_STSPHSRX    BIT(5)                  /* DMA mode: this transfer-complete is a control status stage */
#define INT_STPKTRX     BIT(15)                 /* DMA mode: this transfer-complete belongs to a SETUP packet */

#define EP_IN           1
#define EP_OUT          2
#define EP_NOTIFY       3
#define MPS             64
#define TX_MAX          8128u                   /* bytes per transfer: 127 packets (the packet count field is 7 bits) */
#define RXB             2048u                   /* bulk OUT DMA buffer: 32 packets, two in ping-pong (4 KB: no gain) */
#define RXF_W           104u                    /* FIFO RAM split (256 words): RX, EP0 TX 16, bulk IN TX, notify TX 8, */
#define TXF1_W          112u                    /* and the top 2 x endpoints words for the core's DMA endpoint info (GDFIFOCFG) */
#define EPINFO_W        (2u * USB_OTG_EPS)      /* as TinyUSB's dwc2 driver */

/* ---- descriptors ---- */
static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, MPS,   /* USB 2.0, class CDC, EP0 64 bytes */
    0x09, 0x12, 0x01, 0x00,                     /* VID 0x1209 / PID 0x0001: pid.codes test ID (private testing) */
    0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t cfg_desc[67] = {
    9, 2, 67, 0, 2, 1, 0, 0x80, 50,             /* 2 interfaces, bus powered, 100 mA */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,         /* interface 0: CDC communication, ACM, AT commands */
    5, 0x24, 0x00, 0x10, 0x01,                  /* header, CDC 1.10 */
    5, 0x24, 0x01, 0x00, 1,                     /* call management */
    4, 0x24, 0x02, 0x02,                        /* ACM: line coding + control line state */
    5, 0x24, 0x06, 0, 1,                        /* union: master 0, slave 1 */
    7, 5, 0x80 | EP_NOTIFY, 0x03, 8, 0, 16,     /* notification: interrupt IN, 8 bytes, 16 ms */
    9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,         /* interface 1: CDC data */
    7, 5, EP_OUT, 0x02, MPS, 0, 0,              /* bulk OUT */
    7, 5, 0x80 | EP_IN, 0x02, MPS, 0, 0,        /* bulk IN */
};
static const char *const strings[] = { "esp-ndf", "esp-ndf CDC-ACM", "0001" };

/* ---- state ---- */
static uint8_t setup[8];
static uint8_t setup_dma[8] __attribute__((aligned(4)));         /* DMA target: one SETUP */
static uint8_t ep0_rx[MPS] __attribute__((aligned(4)));          /* control OUT data stage */
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };   /* 115200 8N1 (the host may set anything) */
static uint8_t ep0_buf[256] __attribute__((aligned(4)));
static const uint8_t *ep0_src; static uint32_t ep0_left; static bool ep0_zlp, ep0_out_data;
static uint8_t tx_buf[2][TX_MAX] __attribute__((aligned(4)));   /* double buffer: one on the bus, one being filled */
static uint32_t tx_len[2]; static int tx_cur = -1, tx_q = -1; static bool tx_full_end;
static uint8_t rx_buf[2][RXB] __attribute__((aligned(4)));      /* bulk OUT ping-pong; the core DMA-writes the armed one */
static uint32_t rx_len[2]; static bool rx_full[2]; static int rx_arm_b = -1, rx_rd; static uint32_t rx_pos;
static bool configured, started;
static usb_stats_t st;

static void fifo_flush(void)                      /* all TX FIFOs and the RX FIFO; bounded (1 ms) */
{
    R(GRSTCTL) = 0x10u << 6 | BIT(5) | BIT(4);
    uint64_t t = time_us();
    while ((R(GRSTCTL) & (BIT(5) | BIT(4))) && time_us() - t < 1000) {}
}
static void ep0_out_arm(void)
{
    if (R(DOEPCTL(0)) & EP_ENA) return;            /* already waiting (core rev >= 3.00a keeps it enabled) */
    R(DOEPTSIZ(0)) = 1u << 29 | 1u << 19 | 8;     /* one SETUP of 8 bytes */
    R(DOEPDMA(0)) = (uint32_t)setup_dma;
    R(DOEPCTL(0)) |= EP_ENA | EP_CNAK;
}
static void ep0_out_data_arm(void)                /* control OUT data stage (SET_LINE_CODING) into ep0_rx */
{
    R(DOEPDMA(0)) = (uint32_t)ep0_rx;
    R(DOEPTSIZ(0)) = 1u << 19 | MPS;
    R(DOEPCTL(0)) |= EP_ENA | EP_CNAK;
}
static void ep0_chunk(void)
{
    uint32_t n = ep0_left < MPS ? ep0_left : MPS;
    R(DIEPDMA(0)) = (uint32_t)ep0_src;            /* ep0_buf + k * 64: aligned */
    R(DIEPTSIZ(0)) = 1u << 19 | n;
    R(DIEPCTL(0)) |= EP_ENA | EP_CNAK;
    ep0_src += n; ep0_left -= n;
    if (n < MPS) ep0_zlp = false;                 /* a short packet ends the data stage */
}
static void ep0_send(const void *data, uint32_t len, uint32_t wlength)
{
    if (len > wlength) len = wlength;
    memcpy(ep0_buf, data, len);
    ep0_src = ep0_buf; ep0_left = len; ep0_zlp = len < wlength && len % MPS == 0;
    ep0_chunk();
}
static void ep0_status_in(void)                  /* ZLP; the DMA address must still be aligned (the core advanced it by the last length) */
{
    R(DIEPDMA(0)) = (uint32_t)ep0_buf; R(DIEPTSIZ(0)) = 1u << 19; R(DIEPCTL(0)) |= EP_ENA | EP_CNAK;
}
static void ep0_stall(void) { R(DIEPCTL(0)) |= EP_STALL; R(DOEPCTL(0)) |= EP_STALL; st.stalls++; }

static void rx_arm(int b)                         /* hand one OUT buffer to the core */
{
    rx_arm_b = b;
    R(DOEPDMA(EP_OUT)) = (uint32_t)rx_buf[b];
    R(DOEPTSIZ(EP_OUT)) = (RXB / MPS) << 19 | RXB;
    R(DOEPCTL(EP_OUT)) |= EP_ENA | EP_CNAK;
}
static void rx_done(void)                         /* transfer complete (32 packets or a short one) */
{
    int b = rx_arm_b;
    uint32_t n = RXB - (R(DOEPTSIZ(EP_OUT)) & 0x7FFFF);
    st.rx_bytes += n; rx_arm_b = -1;
    if (!n) { rx_arm(b); return; }                /* zero-length packet: nothing to keep */
    rx_len[b] = n; rx_full[b] = true;
    if (!rx_full[1 - b]) rx_arm(1 - b);           /* the other buffer at once; else NAK until read */
}
static void set_configuration(int cfg)
{
    configured = cfg == 1;
    if (!configured) return;
    R(DIEPCTL(EP_IN)) = EP_SD0PID | 1u << 22 | 2u << 18 | EP_ACTIVE | MPS;      /* bulk, TX FIFO 1 */
    R(DIEPCTL(EP_NOTIFY)) = EP_SD0PID | 2u << 22 | 3u << 18 | EP_ACTIVE | 8;    /* interrupt, TX FIFO 2 */
    R(DOEPCTL(EP_OUT)) = EP_SD0PID | 2u << 18 | EP_ACTIVE | MPS;                /* bulk */
    tx_cur = tx_q = -1; rx_full[0] = rx_full[1] = false; rx_rd = 0; rx_pos = 0; rx_arm(0);
}
static void handle_setup(void)
{
    st.setups++;
    uint8_t rt = setup[0], rq = setup[1];
    uint16_t val = (uint16_t)(setup[2] | setup[3] << 8), wlen = (uint16_t)(setup[6] | setup[7] << 8);
    if ((rt & 0x60) == 0x20) {                    /* CDC class requests */
        if (rq == 0x20) { ep0_out_data = true; return; }                 /* SET_LINE_CODING: data stage follows */
        if (rq == 0x21) { ep0_send(line_coding, 7, wlen); return; }      /* GET_LINE_CODING */
        if (rq == 0x22) { ep0_status_in(); return; }                     /* SET_CONTROL_LINE_STATE */
        ep0_stall(); return;
    }
    switch (rq) {
    case 0x00: { static const uint8_t z[2]; ep0_send(z, 2, wlen); return; }   /* GET_STATUS */
    case 0x01: case 0x03: ep0_status_in(); return;                             /* CLEAR / SET_FEATURE */
    case 0x05: R(DCFG) = (R(DCFG) & ~(0x7Fu << 4)) | (uint32_t)(val & 0x7F) << 4; ep0_status_in(); return;   /* SET_ADDRESS */
    case 0x06:                                                                 /* GET_DESCRIPTOR */
        if (val >> 8 == 1) { ep0_send(dev_desc, sizeof dev_desc, wlen); return; }
        if (val >> 8 == 2) { ep0_send(cfg_desc, sizeof cfg_desc, wlen); return; }
        if (val >> 8 == 3) {
            uint8_t s[64]; int i = val & 0xFF;
            if (i == 0) { s[0] = 4; s[1] = 3; s[2] = 0x09; s[3] = 0x04; ep0_send(s, 4, wlen); return; }   /* English (US) */
            if (i > 3) break;
            const char *t = strings[i - 1]; int n = 0;
            while (t[n] && n < 30) { s[2 + 2 * n] = (uint8_t)t[n]; s[3 + 2 * n] = 0; n++; }
            s[0] = (uint8_t)(2 + 2 * n); s[1] = 3; ep0_send(s, s[0], wlen); return;
        }
        break;                                    /* device qualifier etc.: a full-speed-only device stalls */
    case 0x08: { uint8_t c = configured; ep0_send(&c, 1, wlen); return; }       /* GET_CONFIGURATION */
    case 0x09: set_configuration(val & 0xFF); ep0_status_in(); return;          /* SET_CONFIGURATION */
    case 0x0A: { static const uint8_t z; ep0_send(&z, 1, wlen); return; }       /* GET_INTERFACE */
    case 0x0B: ep0_status_in(); return;                                         /* SET_INTERFACE */
    }
    ep0_stall();
}

static void bus_reset(void)
{
    st.resets++; configured = false; tx_cur = tx_q = -1; rx_arm_b = -1;
    R(DCFG) &= ~(0x7Fu << 4);
    for (int n = 0; n < 4; n++) R(DOEPCTL(n)) |= EP_SNAK;
    R(DAINTMSK) = 0xFFFFFFFFu; R(DIEPMSK) = INT_XFRC; R(DOEPMSK) = INT_XFRC | INT_STUP;
    fifo_flush();
    ep0_out_arm();
}

static void tx_start(int b)
{
    uint32_t n = tx_len[b];
    tx_cur = b; tx_full_end = n % MPS == 0;
    R(DIEPDMA(EP_IN)) = (uint32_t)tx_buf[b];
    R(DIEPTSIZ(EP_IN)) = (n + MPS - 1) / MPS << 19 | n;
    R(DIEPCTL(EP_IN)) |= EP_ENA | EP_CNAK;
}

void usb_dev_poll(void)
{
    if (!started) return;
    uint32_t c0 = ccount(), s = R(GINTSTS);
    if (NDF_USB_GUARD_MS && c0 - guard_fed > 24000000u) { rtc_wdt_feed(); guard_fed = c0; }   /* every 100 ms */
    if (!(s & (GINT_USBRST | GINT_ENUMDNE | GINT_IEPINT | GINT_OEPINT))) return;   /* nothing to do (not counted) */
    if (s & GINT_USBRST) { R(GINTSTS) = GINT_USBRST; bus_reset(); }
    if (s & GINT_ENUMDNE) { R(GINTSTS) = GINT_ENUMDNE; R(DIEPCTL(0)) &= ~3u; ep0_out_arm(); }   /* EP0 64 bytes */
    if (s & GINT_OEPINT) {
        uint32_t d = R(DAINT) >> 16;
        if (d & 1) {
            uint32_t i = R(DOEPINT(0)); R(DOEPINT(0)) = i;
            if (i & INT_STUP) {                       /* SETUP done (as TinyUSB): drop a pending EP0 IN, take the packet */
                if (R(DIEPCTL(0)) & EP_ENA) { R(DIEPCTL(0)) |= EP_SNAK | BIT(30); ep0_left = 0; ep0_zlp = false; }   /* EPDIS */
                memcpy(setup, setup_dma, 8);
                handle_setup();
                if (ep0_out_data) ep0_out_data_arm(); else ep0_out_arm();
            } else if (i & (INT_STPKTRX | INT_STSPHSRX)) {                      /* SETUP / status bookkeeping: nothing to do */
            } else if ((i & INT_XFRC) && ep0_out_data) {
                if (MPS - (R(DOEPTSIZ(0)) & 0x7F)) {      /* data stage, only if bytes arrived: in DMA mode the SETUP stage */
                    memcpy(line_coding, ep0_rx, 7);       /* also ends with a transfer-complete */
                    ep0_out_data = false; ep0_status_in(); ep0_out_arm();
                }
            } else if (i & INT_XFRC) ep0_out_arm();                               /* status stage of a control IN */
        }
        if (d & (1u << EP_OUT)) {
            uint32_t i = R(DOEPINT(EP_OUT)); R(DOEPINT(EP_OUT)) = i;
            if ((i & INT_XFRC) && rx_arm_b >= 0) rx_done();
        }
    }
    if (s & GINT_IEPINT) {
        uint32_t d = R(DAINT) & 0xFFFF;
        if (d & 1) {
            uint32_t i = R(DIEPINT(0)); R(DIEPINT(0)) = i;
            if ((i & INT_XFRC) && (ep0_left || ep0_zlp)) ep0_chunk();
            else if (i & INT_XFRC) ep0_out_arm();     /* IN data / status done: wait for the next SETUP */
        }
        if (d & (1u << EP_IN)) {
            uint32_t i = R(DIEPINT(EP_IN)); R(DIEPINT(EP_IN)) = i;
            if ((i & INT_XFRC) && tx_cur >= 0) {
                st.tx_xfers++;
                if (tx_q >= 0) { int b = tx_q; tx_q = -1; tx_start(b); }                    /* next buffer at once: no idle slot */
                else if (tx_full_end) { tx_full_end = false; R(DIEPDMA(EP_IN)) = (uint32_t)tx_buf[0]; R(DIEPTSIZ(EP_IN)) = 1u << 19; R(DIEPCTL(EP_IN)) |= EP_ENA | EP_CNAK; }   /* stream pauses on a full packet: ZLP closes the host's read */
                else tx_cur = -1;
            }
        }
        if (d & (1u << EP_NOTIFY)) R(DIEPINT(EP_NOTIFY)) = R(DIEPINT(EP_NOTIFY));
    }
    st.poll_cycles += ccount() - c0;
}

int usb_dev_start(void)
{
    if (started) return 0;
    if (NDF_USB_GUARD_MS && rtc_wdt_start(NDF_USB_GUARD_MS, true)) return -1;   /* before the PHY leaves USB-JTAG */
    guard_fed = ccount();
    periph_enable(PERIPH_USB);
    REG(USB_WRAP_CONF) = BIT(31) | BIT(18) | BIT(0);   /* reg clock, pad enable, internal PHY, session-end forced low */
    gpio_in_signal(GPIO_MATRIX_ONE, SIG_USB_IDDIG, false);     /* B-device */
    gpio_in_signal(GPIO_MATRIX_ZERO, SIG_USB_AVALID, false);
    gpio_in_signal(GPIO_MATRIX_ONE, SIG_USB_BVALID, false);    /* no VBUS sense pin: session always valid */
    gpio_in_signal(GPIO_MATRIX_ONE, SIG_USB_VBUSVALID, false);
    gpio_in_signal(GPIO_MATRIX_ZERO, SIG_USB_SESSEND, false);
    REG(RTC_USB_CONF) |= BIT(20) | BIT(19);           /* PHY to OTG: USB Serial/JTAG leaves the bus here */

    uint64_t t = time_us();                           /* every wait below is bounded: 10 ms */
    while (!(R(GRSTCTL) & BIT(31))) if (time_us() - t > 10000) return -1;   /* AHB idle */
    R(GUSBCFG) = (R(GUSBCFG) & ~(0xFu << 10)) | BIT(30) | BIT(6) | 5u << 10;   /* force device mode, full-speed serial PHY, turnaround 5 (3..9 measured: no effect) */
    R(GRSTCTL) |= BIT(0);                             /* core soft reset */
    for (t = time_us(); R(GRSTCTL) & BIT(0); ) if (time_us() - t > 10000) return -2;
    for (t = time_us(); !(R(GRSTCTL) & BIT(31)); ) if (time_us() - t > 10000) return -3;
    delay_ms(25);                                     /* mode change settles */
    R(GOTGCTL) |= BIT(7) | BIT(6);                    /* B-session valid override */
    R(PCGCCTL) = 0;
    R(DCTL) |= BIT(1);                                /* stay disconnected while configuring */
    R(DCFG) = (R(DCFG) & ~0x7FFu) | 3u;               /* full speed, internal 48 MHz PHY; address 0 */
    R(GRXFSIZ) = RXF_W;
    R(GNPTXFSIZ) = 16u << 16 | RXF_W;
    R(DIEPTXF(1)) = TXF1_W << 16 | (RXF_W + 16);
    R(DIEPTXF(2)) = 8u << 16 | (RXF_W + 16 + TXF1_W);
    R(GDFIFOCFG) = (USB_OTG_FIFO_WORDS - EPINFO_W) << 16 | (USB_OTG_FIFO_WORDS - EPINFO_W);   /* DMA info area at the top; before DMAEN */
    fifo_flush();
    R(GINTSTS) = 0xFFFFFFFFu;
    R(GINTMSK) = GINT_USBRST | GINT_ENUMDNE | GINT_IEPINT | GINT_OEPINT;
    R(GAHBCFG) = BIT(5) | 3u << 1;                    /* buffer DMA, INCR4 bursts; interrupt line unused (polled) */
    memset(&st, 0, sizeof st); configured = false; tx_cur = tx_q = -1; rx_arm_b = -1;
    started = true;
    R(DCTL) &= ~BIT(1);                               /* connect: D+ pull-up on */
    return 0;
}

void usb_dev_stop(void)
{
    if (!started) return;
    R(DCTL) |= BIT(1);                                /* disconnect */
    delay_ms(20);
    started = false; configured = false;
    REG(RTC_USB_CONF) &= ~(BIT(20) | BIT(19));        /* PHY back to USB Serial/JTAG (eFuse default) */
    periph_disable(PERIPH_USB);
    if (NDF_USB_GUARD_MS) rtc_wdt_stop();             /* the link is back: guard off */
}

bool usb_cdc_ready(void) { return configured; }
bool usb_cdc_tx_idle(void) { return tx_cur < 0; }

size_t usb_cdc_read(void *buf, size_t max)
{
    uint8_t *p = buf; size_t n = 0; uint32_t c0 = ccount();
    while (n < max) {
        int b = rx_rd; uint32_t avail;
        if (rx_full[b]) avail = rx_len[b];
        else if (rx_arm_b == b) avail = RXB - (R(DOEPTSIZ(EP_OUT)) & 0x7FFFF);   /* packets the core already wrote */
        else break;
        if (rx_pos >= avail) {
            if (!rx_full[b]) break;               /* waiting for the core */
            rx_full[b] = false; rx_pos = 0; rx_rd = 1 - b;
            if (rx_arm_b < 0) rx_arm(b);          /* was NAKing: this buffer is free again */
            continue;
        }
        uint32_t m = avail - rx_pos; if (m > max - n) m = (uint32_t)(max - n);
        memcpy(p + n, rx_buf[b] + rx_pos, m); rx_pos += m; n += m;
    }
    if (n) st.poll_cycles += ccount() - c0;
    return n;
}

size_t usb_cdc_write(const void *buf, size_t n)
{
    if (!configured || tx_q >= 0 || !n) return 0;       /* both buffers in use */
    if (n > TX_MAX) n = TX_MAX;
    int b = tx_cur < 0 ? 0 : 1 - tx_cur;
    uint32_t c0 = ccount();
    memcpy(tx_buf[b], buf, n); tx_len[b] = (uint32_t)n; st.tx_bytes += n;
    if (tx_cur < 0) tx_start(b); else tx_q = b;
    st.poll_cycles += ccount() - c0;
    return n;
}

void usb_get_stats(usb_stats_t *s) { *s = st; }
