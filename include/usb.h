/* USB OTG full-speed device (TRM ch. 32, DWC2 core), CDC-ACM function: the host sees a serial port (/dev/ttyACMx,
 * no driver needed). The S3's USB is full speed only (12 Mbit/s, ~1.2 MB/s bulk at most); there is no high speed.
 * The internal PHY is shared with USB Serial/JTAG: usb_dev_start takes it (the USB-JTAG console and flashing drop
 * off the bus), usb_dev_stop gives it back. Buffer-DMA mode; polled: call usb_dev_poll often (no interrupt yet).
 * Guard: usb_dev_start arms the RTC watchdog (RTC reset, NDF_USB_GUARD_MS, default 3 s), usb_dev_poll feeds it and
 * usb_dev_stop disarms it: if the app stops polling (hang, halted panic), the chip resets with the PHY back on
 * USB-JTAG. Apps that use the RTC watchdog themselves set NDF_USB_GUARD_MS 0. */
#pragma once
#include "ndf.h"

int    usb_dev_start(void);                   /* PHY to OTG, core init, connect (D+ pull-up); 0 on success */
void   usb_dev_stop(void);                    /* disconnect, PHY back to USB Serial/JTAG */
void   usb_dev_poll(void);                    /* service the core: enumeration, control requests, endpoints */
bool   usb_cdc_ready(void);                   /* configured by the host */
size_t usb_cdc_read(void *buf, size_t max);   /* bytes received on the bulk OUT endpoint, never waits */
size_t usb_cdc_write(const void *buf, size_t n);   /* queue up to n bytes on bulk IN; returns bytes taken (0 = busy) */
bool   usb_cdc_tx_idle(void);                 /* the last write has gone out */

typedef struct { uint32_t resets, setups, stalls, rx_bytes, tx_bytes, tx_xfers; uint64_t poll_cycles; } usb_stats_t;   /* poll_cycles: CPU cycles the driver spent working (polls with an event, read/write copies) */
void   usb_get_stats(usb_stats_t *s);
