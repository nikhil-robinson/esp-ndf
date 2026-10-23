#!/usr/bin/env python3
"""Host-side check for the USB CDC download test: same 'S' + count stream as tools/usb_cdc_test.py, read with raw
os.read (1 MB buffers, tty in raw mode) instead of pyserial, to see whether the host read loop limits the number.
Assumes the device is already in its OTG window (start it with usb_cdc_test.py or the 'otg' console command)."""
import os, glob, time, struct, termios, tty, sys
N = int(float(sys.argv[1]) * 1e6) if len(sys.argv) > 1 else 8000000
p = glob.glob('/dev/serial/by-id/usb-esp-ndf*')[0]
fd = os.open(p, os.O_RDWR | os.O_NOCTTY); tty.setraw(fd); termios.tcflush(fd, termios.TCIOFLUSH)
os.write(fd, b'S' + struct.pack('<I', N)); t = time.time(); got = 0; first = None
while got < N:
    b = os.read(fd, 1 << 20)
    if first is None: first = time.time()
    got += len(b)
dt = time.time() - first
print('raw read: %d bytes in %.3f s = %.0f KB/s (timed from the first byte)' % (got, dt, got / dt / 1e3))
