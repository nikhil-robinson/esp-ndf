/* USB OTG full-speed device, CDC-ACM (usb.h). DWC2 core in slave (FIFO) mode, polled.
 * Registers per TRM ch. 32 (DWC2 OTG_FS); PHY switch per ch. 10 (RTC_CNTL_USB_CONF) and ch. 33.
 * Endpoints: EP0 control, EP1 IN bulk (data to host), EP2 OUT bulk (data from host), EP3 IN interrupt (CDC
 * notifications; required by the class, never used). FIFO RAM 256 words: RX 100, EP0 TX 16, EP1 TX 128, EP3 TX 12. */
#include "usb.h"
#include "clk.h"
#include "gpio.h"
#include "sys.h"
#include <string.h>

#define OTG             0x60080000u
#define R(o)            REG(OTG + (o))
#define GOTGCTL         0x000
#define GAHBCFG         0x008
#define GUSBCFG         0x00C
#define GRSTCTL         0x010
#define GINTSTS         0x014
#define GINTMSK         0x018
#define GRXSTSP         0x020
#define GRXFSIZ         0x024
#define GNPTXFSIZ       0x028
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
#define DTXFSTS(n)      (0x918 + 0x20 * (n))
#define DOEPCTL(n)      (0xB00 + 0x20 * (n))
#define DOEPINT(n)      (0xB08 + 0x20 * (n))
#define DOEPTSIZ(n)     (0xB10 + 0x20 * (n))
#define PCGCCTL         0xE00
#define FIFO(n)         (OTG + 0x1000u * ((n) + 1))

#define GINT_RXFLVL     BIT(4)
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

#define RTC_USB_CONF    0x60008120u             /* bit 20: software selects the PHY, bit 19: PHY to OTG */
#define USB_WRAP_CONF   0x60039000u
#define SIG_IDDIG       58
#define SIG_AVALID      59
#define SIG_BVALID      60
#define SIG_VBUSVALID   61
#define SIG_SESSEND     62
#define MATRIX_ONE      0x38
#define MATRIX_ZERO     0x3C

#define EP_IN           1
#define EP_OUT          2
#define EP_NOTIFY       3
#define MPS             64
#define TX_MAX          4096u                   /* bytes per usb_cdc_write (64 packets; the packet count field is 7 bits) */
#define RX_RING         2048u                   /* power of 2 */

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
static uint8_t setup[8] __attribute__((aligned(4)));
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };   /* 115200 8N1 (the host may set anything) */
static uint8_t ep0_buf[256] __attribute__((aligned(4)));
static const uint8_t *ep0_src; static uint32_t ep0_left; static bool ep0_zlp, ep0_out_data;
static uint8_t tx_buf[TX_MAX] __attribute__((aligned(4)));
static uint32_t tx_words, tx_pushed; static volatile bool tx_busy; static bool tx_zlp;
static uint8_t rx_ring[RX_RING]; static uint32_t rx_head, rx_tail; static bool rx_armed;
static bool configured, started;
static usb_stats_t st;

static void fifo_write(int ep, const void *src, uint32_t n)
{
    const uint8_t *p = src;
    for (uint32_t k = 0; k < n; k += 4) { uint32_t w; memcpy(&w, p + k, 4); REG(FIFO(ep)) = w; }   /* caller's buffer has 4-byte slack */
}
static void fifo_read(void *dst, uint32_t n)
{
    uint8_t *p = dst;
    for (uint32_t k = 0; k < n; k += 4) { uint32_t w = REG(FIFO(0)); uint32_t m = n - k < 4 ? n - k : 4; memcpy(p + k, &w, m); }
}

static void ep0_out_arm(void)
{
    R(DOEPTSIZ(0)) = 3u << 29 | 1u << 19 | MPS;   /* 3 back-to-back SETUPs, one packet */
    R(DOEPCTL(0)) |= EP_ENA | EP_CNAK;
}
static void ep0_chunk(void)
{
    uint32_t n = ep0_left < MPS ? ep0_left : MPS;
    R(DIEPTSIZ(0)) = 1u << 19 | n;
    R(DIEPCTL(0)) |= EP_ENA | EP_CNAK;
    if (n) fifo_write(0, ep0_src, n);
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
static void ep0_status_in(void) { R(DIEPTSIZ(0)) = 1u << 19; R(DIEPCTL(0)) |= EP_ENA | EP_CNAK; }
static void ep0_stall(void) { R(DIEPCTL(0)) |= EP_STALL; R(DOEPCTL(0)) |= EP_STALL; st.stalls++; }

static void rx_arm(void)
{
    uint32_t pk = (RX_RING - (rx_head - rx_tail)) / MPS;   /* packets the ring can take */
    if (!pk) { rx_armed = false; return; }        /* full: NAK until read */
    if (pk > 16) pk = 16;
    R(DOEPTSIZ(EP_OUT)) = pk << 19 | pk * MPS;
    R(DOEPCTL(EP_OUT)) |= EP_ENA | EP_CNAK;
    rx_armed = true;
}
static void set_configuration(int cfg)
{
    configured = cfg == 1;
    if (!configured) return;
    R(DIEPCTL(EP_IN)) = EP_SD0PID | 1u << 22 | 2u << 18 | EP_ACTIVE | MPS;      /* bulk, TX FIFO 1 */
    R(DIEPCTL(EP_NOTIFY)) = EP_SD0PID | 2u << 22 | 3u << 18 | EP_ACTIVE | 8;    /* interrupt, TX FIFO 2 */
    R(DOEPCTL(EP_OUT)) = EP_SD0PID | 2u << 18 | EP_ACTIVE | MPS;                /* bulk */
    tx_busy = false; rx_head = rx_tail = 0; rx_arm();
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
    st.resets++; configured = false; tx_busy = false;
    R(DCFG) &= ~(0x7Fu << 4);
    for (int n = 0; n < 4; n++) R(DOEPCTL(n)) |= EP_SNAK;
    R(DAINTMSK) = 0xFFFFFFFFu; R(DIEPMSK) = INT_XFRC; R(DOEPMSK) = INT_XFRC | INT_STUP;
    R(GRSTCTL) = 0x10u << 6 | BIT(5) | BIT(4);    /* flush all TX FIFOs and the RX FIFO */
    while (R(GRSTCTL) & (BIT(5) | BIT(4))) {}
    ep0_out_arm();
}

static void tx_push(void)                         /* whole packets only: the core sends what a complete packet write leaves */
{
    while (tx_pushed < tx_words) {
        uint32_t n = tx_words - tx_pushed;
        if (n > MPS / 4) n = MPS / 4;
        if ((R(DTXFSTS(EP_IN)) & 0xFFFF) < n) return;
        const uint32_t *w = (const uint32_t *)tx_buf + tx_pushed;
        for (uint32_t k = 0; k < n; k++) REG(FIFO(EP_IN)) = w[k];
        tx_pushed += n;
    }
}

void usb_dev_poll(void)
{
    if (!started) return;
    uint32_t s = R(GINTSTS);
    if (s & GINT_USBRST) { R(GINTSTS) = GINT_USBRST; bus_reset(); }
    if (s & GINT_ENUMDNE) { R(GINTSTS) = GINT_ENUMDNE; R(DIEPCTL(0)) &= ~3u; ep0_out_arm(); }   /* EP0 64 bytes */
    while (R(GINTSTS) & GINT_RXFLVL) {
        uint32_t x = R(GRXSTSP), ep = x & 0xF, n = (x >> 4) & 0x7FF, pk = (x >> 17) & 0xF;
        if (pk == 6) fifo_read(setup, 8);                                         /* SETUP data */
        else if (pk == 2 && ep == 0) fifo_read(ep0_buf, n);                       /* control OUT data */
        else if (pk == 2 && ep == EP_OUT) {                                       /* bulk OUT data -> ring */
            uint8_t tmp[MPS] __attribute__((aligned(4))); fifo_read(tmp, n);
            for (uint32_t k = 0; k < n; k++) rx_ring[(rx_head + k) & (RX_RING - 1)] = tmp[k];
            rx_head += n; st.rx_bytes += n;
        } else if (n) { uint8_t tmp[MPS] __attribute__((aligned(4))); fifo_read(tmp, n); }   /* drop */
    }
    if (s & GINT_OEPINT) {
        uint32_t d = R(DAINT) >> 16;
        if (d & 1) {
            uint32_t i = R(DOEPINT(0)); R(DOEPINT(0)) = i;
            if (i & INT_STUP) { handle_setup(); ep0_out_arm(); }
            else if ((i & INT_XFRC) && ep0_out_data) {                            /* SET_LINE_CODING data arrived */
                memcpy(line_coding, ep0_buf, 7); ep0_out_data = false; ep0_status_in(); ep0_out_arm();
            } else if (i & INT_XFRC) ep0_out_arm();                               /* status stage of a control IN */
        }
        if (d & (1u << EP_OUT)) {
            uint32_t i = R(DOEPINT(EP_OUT)); R(DOEPINT(EP_OUT)) = i;
            if (i & INT_XFRC) rx_arm();
        }
    }
    if (s & GINT_IEPINT) {
        uint32_t d = R(DAINT) & 0xFFFF;
        if (d & 1) {
            uint32_t i = R(DIEPINT(0)); R(DIEPINT(0)) = i;
            if ((i & INT_XFRC) && (ep0_left || ep0_zlp)) ep0_chunk();
        }
        if (d & (1u << EP_IN)) {
            uint32_t i = R(DIEPINT(EP_IN)); R(DIEPINT(EP_IN)) = i;
            if ((i & INT_XFRC) && tx_zlp) { tx_zlp = false; R(DIEPTSIZ(EP_IN)) = 1u << 19; R(DIEPCTL(EP_IN)) |= EP_ENA | EP_CNAK; }   /* end on a full packet: ZLP closes the host's read */
            else if (i & INT_XFRC) { tx_busy = false; st.tx_xfers++; }
        }
        if (d & (1u << EP_NOTIFY)) R(DIEPINT(EP_NOTIFY)) = R(DIEPINT(EP_NOTIFY));
    }
    if (tx_busy) tx_push();
    if (configured && !rx_armed) rx_arm();
}

int usb_dev_start(void)
{
    if (started) return 0;
    periph_enable(PERIPH_USB);
    REG(USB_WRAP_CONF) = BIT(31) | BIT(18) | BIT(0);   /* reg clock, pad enable, internal PHY, session-end forced low */
    gpio_in_signal(MATRIX_ONE, SIG_IDDIG, false);     /* B-device */
    gpio_in_signal(MATRIX_ZERO, SIG_AVALID, false);
    gpio_in_signal(MATRIX_ONE, SIG_BVALID, false);    /* no VBUS sense pin: session always valid */
    gpio_in_signal(MATRIX_ONE, SIG_VBUSVALID, false);
    gpio_in_signal(MATRIX_ZERO, SIG_SESSEND, false);
    REG(RTC_USB_CONF) |= BIT(20) | BIT(19);           /* PHY to OTG: USB Serial/JTAG leaves the bus here */

    uint32_t t = 0;
    while (!(R(GRSTCTL) & BIT(31))) if (++t > 1000000) return -1;   /* AHB idle */
    R(GUSBCFG) |= BIT(30) | BIT(6) | 5u << 10;        /* force device mode, full-speed serial PHY, turnaround 5 */
    R(GRSTCTL) |= BIT(0);                             /* core soft reset */
    for (t = 0; R(GRSTCTL) & BIT(0); ) if (++t > 1000000) return -2;
    for (t = 0; !(R(GRSTCTL) & BIT(31)); ) if (++t > 1000000) return -3;
    delay_ms(25);                                     /* mode change settles */
    R(GOTGCTL) |= BIT(7) | BIT(6);                    /* B-session valid override */
    R(PCGCCTL) = 0;
    R(DCTL) |= BIT(1);                                /* stay disconnected while configuring */
    R(DCFG) = (R(DCFG) & ~0x7FFu) | 3u;               /* full speed, internal 48 MHz PHY; address 0 */
    R(GRXFSIZ) = 100;
    R(GNPTXFSIZ) = 16u << 16 | 100;
    R(DIEPTXF(1)) = 128u << 16 | 116;
    R(DIEPTXF(2)) = 12u << 16 | 244;
    R(GRSTCTL) = 0x10u << 6 | BIT(5) | BIT(4);
    while (R(GRSTCTL) & (BIT(5) | BIT(4))) {}
    R(GINTSTS) = 0xFFFFFFFFu;
    R(GINTMSK) = GINT_USBRST | GINT_ENUMDNE | GINT_RXFLVL | GINT_IEPINT | GINT_OEPINT;
    R(GAHBCFG) = 0;                                   /* slave mode, interrupt line unused (polled) */
    memset(&st, 0, sizeof st); configured = false; tx_busy = false; rx_head = rx_tail = 0;
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
}

bool usb_cdc_ready(void) { return configured; }
bool usb_cdc_tx_idle(void) { return !tx_busy; }

size_t usb_cdc_read(void *buf, size_t max)
{
    uint8_t *p = buf; size_t n = 0;
    while (n < max && rx_tail != rx_head) p[n++] = rx_ring[rx_tail++ & (RX_RING - 1)];
    return n;
}

size_t usb_cdc_write(const void *buf, size_t n)
{
    if (!configured || tx_busy || !n) return 0;
    if (n > TX_MAX) n = TX_MAX;
    memcpy(tx_buf, buf, n);
    uint32_t pk = (uint32_t)(n + MPS - 1) / MPS;
    tx_words = (uint32_t)(n + 3) / 4; tx_pushed = 0; tx_busy = true; tx_zlp = n % MPS == 0;
    R(DIEPTSIZ(EP_IN)) = pk << 19 | (uint32_t)n;
    R(DIEPCTL(EP_IN)) |= EP_ENA | EP_CNAK;
    st.tx_bytes += n;
    tx_push();
    return n;
}

void usb_get_stats(usb_stats_t *s) { *s = st; }
