/* USB OTG device, CDC-ACM (usb.h). DWC2 core in buffer-DMA mode (the core moves every packet between RAM
 * and its FIFOs; the CPU only programs transfers), polled.
 * S3: full speed, registers per TRM ch. 32 (DWC2 OTG_FS); PHY switch per ch. 10 (RTC_CNTL_USB_CONF) and ch. 33.
 * P4: the HS controller (0x50000000) on the internal UTMI PHY, high speed (512-byte bulk packets); ESP-IDF v6.1
 * usb_utmi_hal / usb_dwc_ll. It has its own pads, so USB Serial/JTAG stays up and no guard is needed. L2MEM is cached by
 * the L1 D-cache: DMA buffers are 64-byte aligned and written back / invalidated around each transfer.
 * Endpoints: EP0 control, EP1 IN bulk (data to host), EP2 OUT bulk (data from host), EP3 IN interrupt (CDC
 * notifications; required by the class, never used). FIFO RAM 256 words: RX, EP0 TX, EP1 TX, EP3 TX, DMA endpoint info (see RXF_W).
 * DMA buffers are in internal SRAM, 4-byte aligned (no data cache there: nothing to flush). */
#include "usb.h"
#include "soc.h"
#include "clk.h"
#include "gpio.h"
#include "sys.h"
#include "cpu.h"
#if SOC_CPU_RISCV
#include "cache_p4.h"
#endif
#include <string.h>
#include "wdt.h"

/* OTG guard: while the PHY belongs to OTG the USB Serial/JTAG link is gone, so a hang or a halted panic would leave
 * the board unreachable. The RTC watchdog (RTC reset: clears RTC_CNTL_USB_CONF, the PHY select - proven by
 * components/drivers/usb/test_apps/usb_safety_proof) runs for the whole OTG session and is fed from usb_dev_poll(). */
#ifndef NDF_USB_GUARD_MS
#define NDF_USB_GUARD_MS 3000
#endif
/* per-chip packet sizes, FIFO split (words) and DMA buffer handling; the S3 values are the original constants */
#if SOC_CPU_RISCV
#define USB_HS          1
#define BMPS            512                     /* bulk packet size: high speed */
#define IMPS            64                      /* interrupt endpoints */
#define INT_IVL         8                       /* 2^7 x 125 us = 16 ms */
#define HID_IVL         4                       /* 1 ms */
#define USB_DMA_ALIGN   64
#define TX_MAX          16384u
#define RXB_SZ          8192u
#define RXF_W           288u
#define RXF_W_COMP      288u
#define TXF1_W          400u
#define TXF1_W_COMP     256u
#define TXF3_W          256u
#define TXF2_W          16u
#define DMA_TO_DEV(p, n)   cache_dma_writeback((p), (n))
#define DMA_FROM_DEV(p, n) cache_dma_wbinv((p), (n))
#else
#define USB_HS          0
#define BMPS            64
#define IMPS            64
#define INT_IVL         16
#define HID_IVL         1
#define USB_DMA_ALIGN   4
#define TX_MAX          8128u                   /* bytes per transfer: 127 packets (the packet count field is 7 bits) */
#define RXB_SZ          2048u                   /* bulk OUT DMA buffer: 32 packets, two in ping-pong (4 KB: no gain) */
#define RXF_W           104u                    /* FIFO RAM split (256 words): RX, EP0 TX 16, bulk IN TX, notify TX 8, */
#define RXF_W_COMP      88u
#define TXF1_W          112u                    /* and the top 2 x endpoints words for the core's DMA endpoint info (GDFIFOCFG) */
#define TXF1_W_COMP     64u
#define TXF3_W          64u
#define TXF2_W          8u
#define DMA_TO_DEV(p, n)   ((void)0)
#define DMA_FROM_DEV(p, n) ((void)0)
#endif
#if NDF_USB_GUARD_MS
static uint32_t guard_fed;
#endif

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
#define RXB             RXB_SZ
#if USB_HS
#define PIPE_MPS_INIT BMPS,
#else
#define PIPE_MPS_INIT
#endif
#if USB_HS
#define PMPS(q)         ((q)->mps)
#else
#define PMPS(q)         MPS                          /* S3: the constant, as before */
#endif
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
    7, 5, 0x80 | EP_NOTIFY, 0x03, 8, 0, INT_IVL,  /* notification: interrupt IN, 8 bytes, 16 ms */
    9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,         /* interface 1: CDC data */
    7, 5, EP_OUT, 0x02, BMPS & 0xFF, BMPS >> 8, 0,              /* bulk OUT */
    7, 5, 0x80 | EP_IN, 0x02, BMPS & 0xFF, BMPS >> 8, 0,        /* bulk IN */
};
static const char *const strings[] = { "esp-ndf", "esp-ndf CDC-ACM", "0001" };

/* MSC (bulk-only transport, SCSI transparent) and HID (vendor page, 64-byte reports): same endpoints EP1 IN / EP2 OUT */
static const uint8_t msc_dev_desc[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, MPS, 0x09, 0x12, 0x02, 0x00, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t msc_cfg_desc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,         /* mass storage, SCSI transparent, bulk-only */
    7, 5, EP_OUT, 0x02, BMPS & 0xFF, BMPS >> 8, 0,
    7, 5, 0x80 | EP_IN, 0x02, BMPS & 0xFF, BMPS >> 8, 0,
};
static const uint8_t hid_report[] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01,   /* vendor usage page, application collection */
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40,   /* 64 bytes of 0..255 */
    0x09, 0x01, 0x81, 0x02,                     /* input report */
    0x09, 0x01, 0x91, 0x02,                     /* output report */
    0xC0,
};
static const uint8_t hid_dev_desc[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, MPS, 0x09, 0x12, 0x03, 0x00, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t hid_cfg_desc[41] = {
    9, 2, 41, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0x03, 0, 0, 0,               /* HID, no boot protocol */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof hid_report, 0,
    7, 5, 0x80 | EP_IN, 0x03, IMPS, 0, HID_IVL,  /* interrupt IN, 1 ms */
    7, 5, EP_OUT, 0x03, IMPS, 0, HID_IVL,        /* interrupt OUT, 1 ms */
};
/* CDC + MSC composite: IAD around the two CDC interfaces (0, 1), MSC interface 2 on EP4 IN / EP5 OUT */
#define EP_MSC_IN   4
#define EP_MSC_OUT  5
static const uint8_t comp_dev_desc[18] = { 18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, MPS, 0x09, 0x12, 0x04, 0x00, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t comp_cfg_desc[98] = {
    9, 2, 98, 0, 3, 1, 0, 0x80, 50,
    8, 0x0B, 0, 2, 0x02, 0x02, 0x01, 0,           /* IAD: interfaces 0-1 = CDC ACM */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, 0x10, 0x01, 5, 0x24, 0x01, 0x00, 1, 4, 0x24, 0x02, 0x02, 5, 0x24, 0x06, 0, 1,
    7, 5, 0x80 | EP_NOTIFY, 0x03, 8, 0, INT_IVL,
    9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 5, EP_OUT, 0x02, BMPS & 0xFF, BMPS >> 8, 0,
    7, 5, 0x80 | EP_IN, 0x02, BMPS & 0xFF, BMPS >> 8, 0,
    9, 4, 2, 0, 2, 0x08, 0x06, 0x50, 0,           /* interface 2: mass storage */
    7, 5, EP_MSC_OUT, 0x02, BMPS & 0xFF, BMPS >> 8, 0,
    7, 5, 0x80 | EP_MSC_IN, 0x02, BMPS & 0xFF, BMPS >> 8, 0,
};
static int fn = USB_FN_CDC;
void usb_dev_function(int f) { fn = f; }
int usb_dev_get_function(void) { return fn; }

/* ---- state ---- */
static uint8_t setup[8];
static uint8_t setup_dma[USB_DMA_ALIGN > 4 ? 64 : 8] __attribute__((aligned(USB_DMA_ALIGN)));         /* DMA target: one SETUP */
static uint8_t ep0_rx[MPS] __attribute__((aligned(USB_DMA_ALIGN)));          /* control OUT data stage */
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };   /* 115200 8N1 (the host may set anything) */
static uint8_t ep0_buf[256] __attribute__((aligned(USB_DMA_ALIGN)));
static const uint8_t *ep0_src; static uint32_t ep0_left; static bool ep0_zlp, ep0_out_data;
/* A pipe = one bulk IN + OUT endpoint pair with its buffers. Pipe 0: CDC (or the only function), pipe 1: MSC in the
 * composite. TX double buffer (one on the bus, one being filled); RX ping-pong (the core DMA-writes the armed one). */
#define TX2 4096u
typedef struct {
    uint8_t in, out, fifo;
#if USB_HS
    uint16_t mps;                                   /* bulk 512 / interrupt 64 (S3: always MPS, no field) */
#endif
    bool zlp;               /* zlp: stream class, close a full-packet transfer with a ZLP */
    uint8_t *txb[2], *rxb[2]; uint32_t txmax;
    uint32_t tx_len[2]; int tx_cur, tx_q; bool tx_full_end;
    uint32_t rx_len[2]; bool rx_full[2]; int rx_arm_b, rx_rd; uint32_t rx_pos;
} pipe_t;
static uint8_t tx_buf[2][TX_MAX] __attribute__((aligned(USB_DMA_ALIGN))), rx_buf[2][RXB] __attribute__((aligned(USB_DMA_ALIGN)));
static uint8_t tx_buf2[2][TX2] __attribute__((aligned(USB_DMA_ALIGN))), rx_buf2[2][RXB] __attribute__((aligned(USB_DMA_ALIGN)));
static pipe_t pp[2] = {
    { EP_IN, EP_OUT, 1, PIPE_MPS_INIT true, { tx_buf[0], tx_buf[1] }, { rx_buf[0], rx_buf[1] }, TX_MAX, .tx_cur = -1, .tx_q = -1, .rx_arm_b = -1 },
    { EP_MSC_IN, EP_MSC_OUT, 3, PIPE_MPS_INIT false, { tx_buf2[0], tx_buf2[1] }, { rx_buf2[0], rx_buf2[1] }, TX2, .tx_cur = -1, .tx_q = -1, .rx_arm_b = -1 },
};
static int npipes(void) { return fn == USB_FN_CDC_MSC ? 2 : 1; }
static void pipe_reset(pipe_t *q) { q->tx_cur = q->tx_q = -1; q->rx_full[0] = q->rx_full[1] = false; q->rx_rd = 0; q->rx_pos = 0; q->rx_arm_b = -1; }
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
    DMA_FROM_DEV(setup_dma, sizeof setup_dma);
    R(DOEPDMA(0)) = (uint32_t)setup_dma;
    R(DOEPCTL(0)) |= EP_ENA | EP_CNAK;
}
static void ep0_out_data_arm(void)                /* control OUT data stage (SET_LINE_CODING) into ep0_rx */
{
    DMA_FROM_DEV(ep0_rx, sizeof ep0_rx);
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
    DMA_TO_DEV(ep0_buf, sizeof ep0_buf);
    ep0_src = ep0_buf; ep0_left = len; ep0_zlp = len < wlength && len % MPS == 0;
    ep0_chunk();
}
static void ep0_status_in(void)                  /* ZLP; the DMA address must still be aligned (the core advanced it by the last length) */
{
    R(DIEPDMA(0)) = (uint32_t)ep0_buf; R(DIEPTSIZ(0)) = 1u << 19; R(DIEPCTL(0)) |= EP_ENA | EP_CNAK;
}
static void ep0_stall(void) { R(DIEPCTL(0)) |= EP_STALL; R(DOEPCTL(0)) |= EP_STALL; st.stalls++; }

static void rx_arm(pipe_t *q, int b)              /* hand one OUT buffer to the core */
{
    q->rx_arm_b = b;
    DMA_FROM_DEV(q->rxb[b], RXB);
    R(DOEPDMA(q->out)) = (uint32_t)q->rxb[b];
    R(DOEPTSIZ(q->out)) = (RXB / PMPS(q)) << 19 | RXB;
    R(DOEPCTL(q->out)) |= EP_ENA | EP_CNAK;
}
static void rx_done(pipe_t *q)                     /* transfer complete (32 packets or a short one) */
{
    int b = q->rx_arm_b;
    uint32_t n = RXB - (R(DOEPTSIZ(q->out)) & 0x7FFFF);
    st.rx_bytes += n; q->rx_arm_b = -1;
    DMA_FROM_DEV(q->rxb[b], RXB);
    if (!n) { rx_arm(q, b); return; }             /* zero-length packet: nothing to keep */
    q->rx_len[b] = n; q->rx_full[b] = true;
    if (!q->rx_full[1 - b]) rx_arm(q, 1 - b);     /* the other buffer at once; else NAK until read */
}
static void set_configuration(int cfg)
{
    configured = cfg == 1;
    if (!configured) return;
    uint32_t ty = fn == USB_FN_HID ? 3u : 2u;                                  /* HID: interrupt, else bulk */
    pp[0].zlp = fn == USB_FN_CDC || fn == USB_FN_CDC_MSC;                       /* BOT and HID: the length is known */
    if (fn == USB_FN_CDC || fn == USB_FN_CDC_MSC)
        R(DIEPCTL(EP_NOTIFY)) = EP_SD0PID | 2u << 22 | 3u << 18 | EP_ACTIVE | 8;   /* interrupt, TX FIFO 2 */
    for (int k = 0; k < npipes(); k++) {
        pipe_t *q = &pp[k];
#if USB_HS
        q->mps = fn == USB_FN_HID ? IMPS : BMPS;
#endif
        R(DIEPCTL(q->in)) = EP_SD0PID | (uint32_t)q->fifo << 22 | ty << 18 | EP_ACTIVE | PMPS(q);
        R(DOEPCTL(q->out)) = EP_SD0PID | ty << 18 | EP_ACTIVE | PMPS(q);
        pipe_reset(q); rx_arm(q, 0);
    }
}
static void handle_setup(void)
{
    st.setups++;
    uint8_t rt = setup[0], rq = setup[1];
    uint16_t val = (uint16_t)(setup[2] | setup[3] << 8), wlen = (uint16_t)(setup[6] | setup[7] << 8);
    bool msc_if = fn == USB_FN_MSC || (fn == USB_FN_CDC_MSC && (rt & 0x1F) == 1 && setup[4] == 2);   /* composite: by interface */
    if (msc_if && (rt & 0x60) == 0x20) {
        if (rq == 0xFE) { static const uint8_t lun0; ep0_send(&lun0, 1, wlen); return; }   /* GET_MAX_LUN: one LUN */
        if (rq == 0xFF) { extern void usb_msc_reset(void); usb_msc_reset(); ep0_status_in(); return; }   /* BOT reset */
        ep0_stall(); return;
    }
    if (fn == USB_FN_HID) {
        if (rq == 0x06 && (rt & 0x1F) == 1 && val >> 8 == 0x22) { ep0_send(hid_report, sizeof hid_report, wlen); return; }
        if (rq == 0x06 && (rt & 0x1F) == 1 && val >> 8 == 0x21) { ep0_send(hid_cfg_desc + 18, 9, wlen); return; }
        if ((rt & 0x60) == 0x20 && rq == 0x0A) { ep0_status_in(); return; }   /* SET_IDLE */
        if ((rt & 0x60) == 0x20) { ep0_stall(); return; }
    }
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
        if (val >> 8 == 1) { ep0_send(fn == USB_FN_MSC ? msc_dev_desc : fn == USB_FN_HID ? hid_dev_desc : fn == USB_FN_CDC_MSC ? comp_dev_desc : dev_desc, 18, wlen); return; }
        if (val >> 8 == 2) {
            if (fn == USB_FN_MSC) ep0_send(msc_cfg_desc, sizeof msc_cfg_desc, wlen);
            else if (fn == USB_FN_HID) ep0_send(hid_cfg_desc, sizeof hid_cfg_desc, wlen);
            else if (fn == USB_FN_CDC_MSC) ep0_send(comp_cfg_desc, sizeof comp_cfg_desc, wlen);
            else ep0_send(cfg_desc, sizeof cfg_desc, wlen);
            return;
        }
        if (val >> 8 == 3) {
            uint8_t s[64]; int i = val & 0xFF;
            if (i == 0) { s[0] = 4; s[1] = 3; s[2] = 0x09; s[3] = 0x04; ep0_send(s, 4, wlen); return; }   /* English (US) */
            if (i > 3) break;
            const char *t = i == 2 && fn == USB_FN_MSC ? "esp-ndf disk" : i == 2 && fn == USB_FN_HID ? "esp-ndf HID" : strings[i - 1]; int n = 0;
            while (t[n] && n < 30) { s[2 + 2 * n] = (uint8_t)t[n]; s[3 + 2 * n] = 0; n++; }
            s[0] = (uint8_t)(2 + 2 * n); s[1] = 3; ep0_send(s, s[0], wlen); return;
        }
#if USB_HS
        if (val >> 8 == 6) {                      /* device qualifier: the other speed (full speed) has the same class, EP0 64 */
            const uint8_t *d = fn == USB_FN_MSC ? msc_dev_desc : fn == USB_FN_HID ? hid_dev_desc : fn == USB_FN_CDC_MSC ? comp_dev_desc : dev_desc;
            const uint8_t q[10] = { 10, 6, 0x00, 0x02, d[4], d[5], d[6], MPS, 1, 0 };
            ep0_send(q, 10, wlen); return;
        }
#endif
        break;                                    /* other-speed configuration etc.: stall */
    case 0x08: { uint8_t c = configured; ep0_send(&c, 1, wlen); return; }       /* GET_CONFIGURATION */
    case 0x09: set_configuration(val & 0xFF); ep0_status_in(); return;          /* SET_CONFIGURATION */
    case 0x0A: { static const uint8_t z; ep0_send(&z, 1, wlen); return; }       /* GET_INTERFACE */
    case 0x0B: ep0_status_in(); return;                                         /* SET_INTERFACE */
    }
    ep0_stall();
}

static void bus_reset(void)
{
    st.resets++; configured = false; pipe_reset(&pp[0]); pipe_reset(&pp[1]);
    R(DCFG) &= ~(0x7Fu << 4);
    for (int n = 0; n < USB_OTG_EPS; n++) R(DOEPCTL(n)) |= EP_SNAK;
    R(DAINTMSK) = 0xFFFFFFFFu; R(DIEPMSK) = INT_XFRC; R(DOEPMSK) = INT_XFRC | INT_STUP;
    fifo_flush();
    ep0_out_arm();
}

static void tx_start(pipe_t *q, int b)
{
    uint32_t n = q->tx_len[b];
    q->tx_cur = b; q->tx_full_end = n % PMPS(q) == 0 && q->zlp;
    DMA_TO_DEV(q->txb[b], (n + 63u) & ~63u);
    R(DIEPDMA(q->in)) = (uint32_t)q->txb[b];
    R(DIEPTSIZ(q->in)) = (n + PMPS(q) - 1) / PMPS(q) << 19 | n;
    R(DIEPCTL(q->in)) |= EP_ENA | EP_CNAK;
}

void usb_dev_poll(void)
{
    if (!started) return;
    uint32_t c0 = ccount(), s = R(GINTSTS);
#if NDF_USB_GUARD_MS
    if (c0 - guard_fed > 24000000u) { rtc_wdt_feed(); guard_fed = c0; }   /* every 100 ms */
#endif
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
        for (int k = 0; k < npipes(); k++) {
            pipe_t *q = &pp[k];
            if (!(d & (1u << q->out))) continue;
            uint32_t i = R(DOEPINT(q->out)); R(DOEPINT(q->out)) = i;
            if ((i & INT_XFRC) && q->rx_arm_b >= 0) rx_done(q);
        }
    }
    if (s & GINT_IEPINT) {
        uint32_t d = R(DAINT) & 0xFFFF;
        if (d & 1) {
            uint32_t i = R(DIEPINT(0)); R(DIEPINT(0)) = i;
            if ((i & INT_XFRC) && (ep0_left || ep0_zlp)) ep0_chunk();
            else if (i & INT_XFRC) ep0_out_arm();     /* IN data / status done: wait for the next SETUP */
        }
        for (int k = 0; k < npipes(); k++) {
            pipe_t *q = &pp[k];
            if (!(d & (1u << q->in))) continue;
            uint32_t i = R(DIEPINT(q->in)); R(DIEPINT(q->in)) = i;
            if ((i & INT_XFRC) && q->tx_cur >= 0) {
                st.tx_xfers++;
                if (q->tx_q >= 0) { int b = q->tx_q; q->tx_q = -1; tx_start(q, b); }          /* next buffer at once: no idle slot */
                else if (q->tx_full_end) { q->tx_full_end = false; R(DIEPDMA(q->in)) = (uint32_t)q->txb[0]; R(DIEPTSIZ(q->in)) = 1u << 19; R(DIEPCTL(q->in)) |= EP_ENA | EP_CNAK; }   /* stream pauses on a full packet: ZLP closes the host's read */
                else q->tx_cur = -1;
            }
        }
        if (d & (1u << EP_NOTIFY)) R(DIEPINT(EP_NOTIFY)) = R(DIEPINT(EP_NOTIFY));
    }
    st.poll_cycles += ccount() - c0;
}

int usb_dev_start(void)
{
    if (started) return 0;
#if SOC_CPU_RISCV
    /* UTMI PHY on: OTG20 system clock, PHY reference clock, reset pulse, "suspendm" high (else detach is not seen),
     * 16-bit low-speed parallel path (usb_utmi_hal_init), data pulldowns off (device) */
    REG_SET(HP_CLKRST_BASE + 0x18, BIT(16));
    REG_SET(LP_AON_CLKRST_USB, BIT(30));
    REG_SET(LP_AON_CLKRST_USB, BIT(1) | BIT(2));
    REG_CLR(LP_AON_CLKRST_USB, BIT(1) | BIT(2));
    REG_SET(HP_SYSTEM_USBOTG20_CTRL, BIT(21));
    REG_SET(USB_UTMI_FC_06, BIT(0) | BIT(3));
    REG_CLR(LP_SYS_USB_PHY_CTRL, BIT(1) | BIT(2));
#else
#if NDF_USB_GUARD_MS
    if (rtc_wdt_start(NDF_USB_GUARD_MS, true)) return -1;   /* before the PHY leaves USB-JTAG */
    guard_fed = ccount();
#endif
    periph_enable(PERIPH_USB);
    REG(USB_WRAP_CONF) = BIT(31) | BIT(18) | BIT(0);   /* reg clock, pad enable, internal PHY, session-end forced low */
    gpio_in_signal(GPIO_MATRIX_ONE, SIG_USB_IDDIG, false);     /* B-device */
    gpio_in_signal(GPIO_MATRIX_ZERO, SIG_USB_AVALID, false);
    gpio_in_signal(GPIO_MATRIX_ONE, SIG_USB_BVALID, false);    /* no VBUS sense pin: session always valid */
    gpio_in_signal(GPIO_MATRIX_ONE, SIG_USB_VBUSVALID, false);
    gpio_in_signal(GPIO_MATRIX_ZERO, SIG_USB_SESSEND, false);
    REG(RTC_USB_CONF) |= BIT(20) | BIT(19);           /* PHY to OTG: USB Serial/JTAG leaves the bus here */
#endif

    uint64_t t = time_us();                           /* every wait below is bounded: 10 ms */
    while (!(R(GRSTCTL) & BIT(31))) if (time_us() - t > 10000) return -1;   /* AHB idle */
#if SOC_CPU_RISCV
    R(GUSBCFG) = (R(GUSBCFG) & ~(0xFu << 10 | BIT(6) | BIT(4))) | BIT(30) | BIT(3) | 5u << 10;   /* force device mode, UTMI+ 16-bit (PHYIf), turnaround 5 */
#else
    R(GUSBCFG) = (R(GUSBCFG) & ~(0xFu << 10)) | BIT(30) | BIT(6) | 5u << 10;   /* force device mode, full-speed serial PHY, turnaround 5 (3..9 measured: no effect) */
#endif
    R(GRSTCTL) |= BIT(0);                             /* core soft reset */
    for (t = time_us(); R(GRSTCTL) & BIT(0); ) if (time_us() - t > 10000) return -2;
    for (t = time_us(); !(R(GRSTCTL) & BIT(31)); ) if (time_us() - t > 10000) return -3;
    delay_ms(25);                                     /* mode change settles */
    R(GOTGCTL) |= BIT(7) | BIT(6);                    /* B-session valid override */
    R(PCGCCTL) = 0;
    R(DCTL) |= BIT(1);                                /* stay disconnected while configuring */
    R(DCFG) = (R(DCFG) & ~0x7FFu) | (USB_HS ? 0u : 3u);   /* high speed (P4) / full speed, internal 48 MHz PHY (S3); address 0 */
    uint32_t rxw = fn == USB_FN_CDC_MSC ? RXF_W_COMP : RXF_W, t1 = fn == USB_FN_CDC_MSC ? TXF1_W_COMP : TXF1_W;   /* composite: room for FIFO 3 */
    R(GRXFSIZ) = rxw;
    R(GNPTXFSIZ) = 16u << 16 | rxw;
    R(DIEPTXF(1)) = t1 << 16 | (rxw + 16);
    R(DIEPTXF(2)) = TXF2_W << 16 | (rxw + 16 + t1);
    R(DIEPTXF(3)) = TXF3_W << 16 | (rxw + 16 + t1 + TXF2_W);
    R(GDFIFOCFG) = (USB_OTG_FIFO_WORDS - EPINFO_W) << 16 | (USB_OTG_FIFO_WORDS - EPINFO_W);   /* DMA info area at the top; before DMAEN */
    fifo_flush();
    R(GINTSTS) = 0xFFFFFFFFu;
    R(GINTMSK) = GINT_USBRST | GINT_ENUMDNE | GINT_IEPINT | GINT_OEPINT;
    R(GAHBCFG) = BIT(5) | 3u << 1;                    /* buffer DMA, INCR4 bursts; interrupt line unused (polled) */
    memset(&st, 0, sizeof st); configured = false; pipe_reset(&pp[0]); pipe_reset(&pp[1]);
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
#if SOC_CPU_RISCV
    REG_CLR(HP_CLKRST_BASE + 0x18, BIT(16));          /* OTG20 system clock and PHY reference off (own pads: nothing to hand back) */
    REG_CLR(LP_AON_CLKRST_USB, BIT(30));
#else
    REG(RTC_USB_CONF) &= ~(BIT(20) | BIT(19));        /* PHY back to USB Serial/JTAG (eFuse default) */
    periph_disable(PERIPH_USB);
#if NDF_USB_GUARD_MS
    rtc_wdt_stop();                                   /* the link is back: guard off */
#endif
#endif
}

bool usb_cdc_ready(void) { return configured; }
bool usb_cdc_tx_idle(void) { return pp[0].tx_cur < 0; }

size_t usb_pipe_read(int k, void *buf, size_t max)
{
    if (!(k >= 0 && k < 2)) return 0;      /* caller index: never past the hardware */
    pipe_t *q = &pp[k];
    uint8_t *p = buf; size_t n = 0; uint32_t c0 = ccount();
    while (n < max) {
        int b = q->rx_rd; uint32_t avail;
        if (q->rx_full[b]) avail = q->rx_len[b];
        else if (q->rx_arm_b == b) avail = RXB - (R(DOEPTSIZ(q->out)) & 0x7FFFF);   /* packets the core already wrote */
        else break;
        if (q->rx_pos >= avail) {
            if (!q->rx_full[b]) break;            /* waiting for the core */
            q->rx_full[b] = false; q->rx_pos = 0; q->rx_rd = 1 - b;
            if (q->rx_arm_b < 0) rx_arm(q, b);    /* was NAKing: this buffer is free again */
            continue;
        }
        uint32_t m = avail - q->rx_pos; if (m > max - n) m = (uint32_t)(max - n);
        DMA_FROM_DEV(q->rxb[b] + (q->rx_pos & ~63u), (m + (q->rx_pos & 63u) + 63u) & ~63u);   /* lines the core just wrote */
        memcpy(p + n, q->rxb[b] + q->rx_pos, m); q->rx_pos += m; n += m;
    }
    if (n) st.poll_cycles += ccount() - c0;
    return n;
}

size_t usb_pipe_write(int k, const void *buf, size_t n)
{
    if (!(k >= 0 && k < 2)) return 0;      /* caller index: never past the hardware */
    pipe_t *q = &pp[k];
    if (!configured || q->tx_q >= 0 || !n) return 0;    /* both buffers in use */
    if (n > q->txmax) n = q->txmax;
    int b = q->tx_cur < 0 ? 0 : 1 - q->tx_cur;
    uint32_t c0 = ccount();
    memcpy(q->txb[b], buf, n); q->tx_len[b] = (uint32_t)n; st.tx_bytes += n;
    if (q->tx_cur < 0) tx_start(q, b); else q->tx_q = b;
    st.poll_cycles += ccount() - c0;
    return n;
}

size_t usb_cdc_read(void *buf, size_t max) { return usb_pipe_read(0, buf, max); }
size_t usb_cdc_write(const void *buf, size_t n) { return usb_pipe_write(0, buf, n); }
int usb_msc_pipe(void) { return fn == USB_FN_CDC_MSC; }

void usb_get_stats(usb_stats_t *s) { *s = st; }
