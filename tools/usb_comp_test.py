#!/usr/bin/env python3
"""Host side of examples/usb_msc 'comp': CDC-ACM + mass storage composite. Checks the disk (INQUIRY vendor, capacity,
FAT serial by udev) and a CDC echo of 64 KB at the same time, then reads the device's counters over USB Serial/JTAG.
Usage: usb_comp_test.py USJ_PORT [secs]"""
import serial, glob, os, sys, time
port, secs = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 20
c = serial.Serial(); c.port = port; c.baudrate = 115200; c.timeout = 0.5; c.dtr = False; c.rts = False; c.open()
time.sleep(0.3); c.write(b'\n'); time.sleep(0.3); c.reset_input_buffer()
c.write(b'comp %d\n' % secs); print(c.readline().decode(errors='replace').strip()); c.close()
fails = 0
def check(name, ok, extra=''):
    global fails; print(('PASS ' if ok else 'FAIL ') + name + (' ' + extra if extra else '')); fails += not ok
blk, acm, t0 = None, None, time.time()
while time.time() - t0 < 15 and not (blk and acm):
    for d in glob.glob('/sys/block/sd*'):
        try:
            if open(d + '/device/vendor').read().strip() == 'esp-ndf': blk = d
        except OSError: pass
    g = glob.glob('/dev/serial/by-id/usb-esp-ndf*')
    if g: acm = g[0]
    time.sleep(0.2)
check('composite: disk enumerated', bool(blk), blk or '')
check('composite: CDC-ACM port', bool(acm), acm or '')
if blk:
    check('capacity 256 x 512 B', int(open(blk + '/size').read()) == 256)
if acm:
    a = serial.Serial(); a.port = acm; a.timeout = 2; a.open()
    data = bytes((i * 7 + 1) & 255 for i in range(65536)); got = b''; t = time.time()
    for o in range(0, len(data), 4096):
        a.write(data[o:o + 4096])
        while len(got) < o + 4096 and time.time() - t < 10: got += a.read(o + 4096 - len(got))
    a.close()
    check('CDC echo 64 KB while the disk is mounted', got == data, '%d B, %.0f KB/s' % (len(got), len(got) / 1024 / (time.time() - t)))
if blk:
    uuid = None
    for _ in range(40):
        if os.path.exists('/dev/disk/by-uuid/1234-ABCD'): uuid = os.path.realpath('/dev/disk/by-uuid/1234-ABCD'); break
        time.sleep(0.25)
    check('FAT serial 1234-ABCD read by udev', uuid == '/dev/' + os.path.basename(blk), str(uuid))
end = time.time() + secs + 20
while time.time() < end and not os.path.exists(port): time.sleep(0.2)
time.sleep(1)
c = serial.Serial(); c.port = port; c.baudrate = 115200; c.timeout = 3; c.dtr = False; c.rts = False; c.open()
for _ in range(6):
    l = c.readline().decode(errors='replace').strip()
    if l: print('device:', l)
    if l.startswith('comp rc'): check('device: rc 0, no SCSI errors', l.startswith('comp rc 0') and ' errors 0' in l); break
print('=== usb_comp %s (%d failures) ===' % ('ALL PASS' if not fails else 'FAILED', fails))
