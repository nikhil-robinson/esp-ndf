/* USB OTG full-speed device (TRM ch. 32, DWC2 core), CDC-ACM function: the host sees a serial port (/dev/ttyACMx,
 * no driver needed). The S3's USB is full speed only (12 Mbit/s, ~1.2 MB/s bulk at most); there is no high speed.
 * The internal PHY is shared with USB Serial/JTAG: usb_dev_start takes it (the USB-JTAG console and flashing drop
 * off the bus), usb_dev_stop gives it back. Buffer-DMA mode; polled: call usb_dev_poll often (no interrupt yet).
 * Guard: usb_dev_start arms the RTC watchdog (RTC reset, NDF_USB_GUARD_MS, default 3 s), usb_dev_poll feeds it and
 * usb_dev_stop disarms it: if the app stops polling (hang, halted panic), the chip resets with the PHY back on
 * USB-JTAG. Apps that use the RTC watchdog themselves set NDF_USB_GUARD_MS 0. */
#pragma once
#include "ndf.h"

/* Function of the device, chosen before usb_dev_start: CDC-ACM (default), mass storage (usb_msc_*), HID (usb_hid_*). */
enum { USB_FN_CDC = 0, USB_FN_MSC, USB_FN_HID, USB_FN_CDC_MSC };   /* CDC_MSC: composite, both at once (PID 0x0004) */
void   usb_dev_function(int fn);
int    usb_dev_get_function(void);
int    usb_dev_start(void);                   /* PHY to OTG, core init, connect (D+ pull-up); 0 on success */
void   usb_dev_stop(void);                    /* disconnect, PHY back to USB Serial/JTAG */
void   usb_dev_poll(void);                    /* service the core: enumeration, control requests, endpoints */
bool   usb_cdc_ready(void);                   /* configured by the host */
size_t usb_cdc_read(void *buf, size_t max);   /* bytes received on the bulk OUT endpoint, never waits */
size_t usb_cdc_write(const void *buf, size_t n);   /* queue up to n bytes on bulk IN; returns bytes taken (0 = busy) */
bool   usb_cdc_tx_idle(void);                 /* the last write has gone out */
/* raw bulk pipes: 0 = CDC / the only function, 1 = MSC in the composite (usb_msc_pipe() says which MSC uses) */
size_t usb_pipe_read(int pipe, void *buf, size_t max);
size_t usb_pipe_write(int pipe, const void *buf, size_t n);
int    usb_msc_pipe(void);

typedef struct { uint32_t resets, setups, stalls, rx_bytes, tx_bytes, tx_xfers; uint64_t poll_cycles; } usb_stats_t;   /* poll_cycles: CPU cycles the driver spent working (polls with an event, read/write copies) */
void   usb_get_stats(usb_stats_t *s);

/* Mass storage (bulk-only transport, SCSI) over any block device, presented as 512-byte blocks.
 * Call usb_msc_poll() after usb_dev_poll() in the loop. Read-only disks: ro = true. */
#include "bdev.h"
int    usb_msc_attach(bdev_t *b, bool ro);
void   usb_msc_poll(void);
void   usb_msc_reset(void);
typedef struct { uint32_t cmds, read_blocks, write_blocks, errors, unsupported; uint8_t last_bad_op; } usb_msc_stats_t;   /* unsupported: opcodes answered ILLEGAL REQUEST (e.g. udev's ATA PASS-THROUGH probe 0xA1), not errors */
void   usb_msc_stats(usb_msc_stats_t *s);

/* HID, vendor usage page, 64-byte input / output reports over interrupt endpoints (1 ms). */
bool   usb_hid_send(const uint8_t report[64]);   /* false: previous report still queued */
bool   usb_hid_recv(uint8_t report[64]);         /* true when an output report arrived */
