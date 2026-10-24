#!/usr/bin/env python3
"""Host side of examples/usb_msc "hid": the HID function enumerates, the kernel's hid core parsed our report
descriptor (sysfs copy matches byte for byte), and the device sent input reports while configured (counter).
/dev/hidraw* is root-only here, so reports are not read by the host. Usage: usb_hid_test.py PORT [secs]"""
import serial, glob, os, sys, time
REPORT = bytes([0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40,
                0x09, 0x01, 0x81, 0x02, 0x09, 0x01, 0x91, 0x02, 0xC0])
port, secs = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 10
c = serial.Serial(); c.port = port; c.baudrate = 115200; c.timeout = 0.5; c.dtr = False; c.rts = False; c.open()
time.sleep(0.3); c.write(b'\n'); time.sleep(0.3); c.reset_input_buffer()
c.write(b'hid %d\n' % secs); print(c.readline().decode(errors='replace').strip()); c.close()
fails = 0
def check(name, ok, extra=''):
    global fails; print(('PASS ' if ok else 'FAIL ') + name + (' ' + extra if extra else '')); fails += not ok
dev, t0 = None, time.time()
while time.time() - t0 < 10 and not dev:
    for d in glob.glob('/sys/bus/hid/devices/*1209:0003*'):
        dev = d
    time.sleep(0.2)
check('HID enumerated (1209:0003)', bool(dev), dev or '')
for u in glob.glob('/sys/bus/usb/devices/*/idProduct'):
    if open(u).read().strip() == '0003' and open(os.path.dirname(u) + '/idVendor').read().strip() == '1209':
        print('   usb:', os.path.dirname(u), 'config', open(os.path.dirname(u) + '/bConfigurationValue').read().strip())
if dev:
    rd = open(dev + '/report_descriptor', 'rb').read()
    check('report descriptor read by the kernel matches', rd == REPORT, '%d bytes' % len(rd))
    check('hidraw node created', bool(glob.glob(dev + '/hidraw/hidraw*')))
end = time.time() + secs + 20
while time.time() < end and not os.path.exists(port): time.sleep(0.2)
time.sleep(1)
c = serial.Serial(); c.port = port; c.baudrate = 115200; c.timeout = 3; c.dtr = False; c.rts = False; c.open()
for _ in range(4):
    l = c.readline().decode(errors='replace').strip()
    if l: print('device:', l)
    if 'hid rc' in l or 'last hid' in l:
        n = int(l.split('sent ')[1].split()[0])
        # usbhid polls interrupt IN only while /dev/hidraw is open (root here): the 2 queue buffers fill and wait
        check('input reports queued (host polls only with hidraw open)', n >= 2, '%d' % n); break
print('=== usb_hid %s (%d failures) ===' % ('ALL PASS' if not fails else 'FAILED', fails))
