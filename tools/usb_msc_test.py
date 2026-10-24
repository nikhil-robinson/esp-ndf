#!/usr/bin/env python3
"""Host side of examples/usb_msc (no root needed): ask for an OTG window, then check what the kernel and udev made of
the disk - vendor/model in sysfs, capacity (READ CAPACITY), and the FAT volume serial found by udev's blkid probe
(/dev/disk/by-uuid/1234-ABCD: the kernel READ(10)s returned the right bytes). Usage: usb_msc_test.py PORT [secs]"""
import serial, glob, os, sys, time
port, secs = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 15
c = serial.Serial(); c.port = port; c.baudrate = 115200; c.timeout = 0.5; c.dtr = False; c.rts = False; c.open()
time.sleep(0.3); c.write(b'\n'); time.sleep(0.3); c.reset_input_buffer()
c.write(b'msc %d\n' % secs); print(c.readline().decode(errors='replace').strip()); c.close()
fails = 0
def check(name, ok, extra=''):
    global fails; print(('PASS ' if ok else 'FAIL ') + name + (' ' + extra if extra else '')); fails += not ok
blk, t0 = None, time.time()
while time.time() - t0 < 15 and not blk:
    for d in glob.glob('/sys/block/sd*'):
        try:
            if open(d + '/device/vendor').read().strip() == 'esp-ndf': blk = d
        except OSError: pass
    time.sleep(0.2)
check('disk enumerated (INQUIRY vendor esp-ndf)', bool(blk), blk or '')
if blk:
    size = int(open(blk + '/size').read())
    check('capacity 256 x 512 B (READ CAPACITY)', size == 256, '%d sectors' % size)
    uuid = None
    for _ in range(40):
        if os.path.exists('/dev/disk/by-uuid/1234-ABCD'): uuid = os.path.realpath('/dev/disk/by-uuid/1234-ABCD'); break
        time.sleep(0.25)
    check('FAT serial 1234-ABCD read by udev blkid (READ(10) data)', uuid == '/dev/' + os.path.basename(blk), str(uuid))
end = time.time() + secs + 20
while time.time() < end and not os.path.exists(port): time.sleep(0.2)
time.sleep(1)
c = serial.Serial(); c.port = port; c.baudrate = 115200; c.timeout = 3; c.dtr = False; c.rts = False; c.open()
for _ in range(4):
    l = c.readline().decode(errors='replace').strip()
    if l: print('device:', l)
    if l.startswith('msc rc') or l.startswith('last msc'): check('device: no SCSI errors', ' errors 0' in l); break
print('=== usb_msc %s (%d failures) ===' % ('ALL PASS' if not fails else 'FAILED', fails))
