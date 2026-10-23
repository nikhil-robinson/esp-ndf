#!/usr/bin/env python3
"""Host side of examples/usb_cdc: hand the PHY to our OTG CDC-ACM device, check enumeration, echo, then measure
download (device -> host) and upload (host -> device) throughput with byte-exact pattern checks; then wait for the
USB Serial/JTAG console to come back and read the device counters. Usage: usb_cdc_test.py [seconds] [MB]"""
import serial, glob, time, struct, sys
IDF = '--idf' in sys.argv; sys.argv = [a for a in sys.argv if a != '--idf']   # IDF sibling: TinyUSB takes the PHY at boot
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 25
NBYTES = int(float(sys.argv[2]) * 1e6) if len(sys.argv) > 2 else 4000000
USJ = glob.glob('/dev/serial/by-id/usb-Espressif_USB_JTAG*')
assert USJ or IDF, 'USB Serial/JTAG port not found'
USJ = USJ[0] if USJ else None
def opn(port, timeout):
    c = serial.Serial(None, 115200, timeout=timeout); c.port = port; c.dtr = False; c.rts = False; c.open()   # DTR/RTS low: no chip reset
    return c
def wait_port(pattern, timeout):
    end = time.time() + timeout
    while time.time() < end:
        g = glob.glob(pattern)
        if g: return g[0]
        time.sleep(0.1)
    return None
if not IDF:
    c = opn(USJ, 0.5)
    end = time.time() + 8                               # sync: the device answers a bare line with 'ok'
    while time.time() < end:
        c.write(b'\n')
        if c.readline().decode(errors='replace').strip() == 'ok': break
    c.write(b'otg %d\n' % SECS); print(c.readline().decode(errors='replace').strip()); c.close()
t0 = time.time()
cdc = wait_port('/dev/serial/by-id/usb-Espressif_Systems*' if IDF else '/dev/serial/by-id/usb-esp-ndf*', 40)
print('CDC device:', cdc, 'after %.2f s' % (time.time() - t0))
if cdc:
    time.sleep(0.3)
    p = opn(cdc, 2)
    msg = bytes(b for b in range(32, 127) if b not in b'SU') * 4; p.write(msg); back = p.read(len(msg))   # S/U start commands
    print('echo %d bytes: %s' % (len(msg), 'OK' if back == msg else 'FAIL (%d back)' % len(back)))
    if back != msg:
        i = next((i for i in range(min(len(msg), len(back))) if msg[i] != back[i]), None)
        print('   first difference at %s: sent %s back %s' % (i, msg[i:i + 12] if i is not None else '', back[i:i + 12] if i is not None else ''))
    pat = bytes(k & 0xFF for k in range(256)) * (NBYTES // 256 + 1)
    p.reset_input_buffer(); p.timeout = 5; p.write(b"S" + struct.pack('<I', NBYTES)); t = time.time(); got = bytearray()
    while len(got) < NBYTES:
        b = p.read(min(65536, NBYTES - len(got)))
        if not b: break
        got += b
    dt = time.time() - t
    bad = next((i for i in range(len(got)) if got[i] != pat[i]), None)
    if bad is not None: print('   first wrong byte at %d: got %s want %s' % (bad, bytes(got[bad:bad + 16]).hex(), pat[bad:bad + 16].hex()))
    print('download %d bytes in %.3f s = %.0f KB/s, pattern %s' % (len(got), dt, len(got) / dt / 1e3, 'OK' if bytes(got) == pat[:len(got)] and len(got) == NBYTES else 'FAIL'))
    try:
        t = time.time(); p.write(b'U' + struct.pack('<I', NBYTES)); p.write(pat[:NBYTES]); r = p.read(8); dt = time.time() - t
        rec, err = struct.unpack('<II', r) if len(r) == 8 else (-1, -1)
        print('upload %d bytes in %.3f s = %.0f KB/s, device received %d errors %d' % (NBYTES, dt, NBYTES / dt / 1e3, rec, err))
    except Exception as e: print('upload stage failed:', e)
port = wait_port('/dev/serial/by-id/usb-Espressif_USB_JTAG*', SECS + 15)
print('USB Serial/JTAG back:', bool(port))
if port:
    c = opn(port, 3); end = time.time() + 6
    while time.time() < end:
        l = c.readline().decode(errors='replace').strip()
        if l: print('device:', l)
        if l.startswith('otg rc'): break
