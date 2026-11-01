#!/usr/bin/env python3
"""GATT server test: the IDF peer (central + GATT client) connects to the esp-ndf server (examples/bluetooth/ble_gatts) and
discovers it, reads and writes a value, subscribes to notifications and checks that none are lost; a link loss must be survived."""
import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf

ADDR = 'c6:55:44:33:22:11'      # advertised as bytes 11 22 33 44 55 C6 (little endian)
ndf = Ndf(ready='advertise rc 0', timeout=20)
p = Peer(); time.sleep(5)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg); 
    if not c: fail.append(msg)

def connect():
    p.cmd('ble_adv_stop', 5)
    try: p.cmd('ble_connect %s 1' % ADDR, 20); return True
    except Exception as e: print('connect:', e); return False

check(connect(), 'central connects to the esp-ndf server')
d = p.cmd('ble_disc', 30)
svc = [l for l in d if l.startswith('svc')]; chr_ = [l for l in d if l.startswith('chr')]; dsc = [l for l in d if l.startswith('dsc')]
check(any('0x1800' in l for l in svc) and any('0x1801' in l for l in svc) and any('0xfff0' in l.lower() for l in svc), 'services 0x1800, 0x1801, 0xfff0 discovered')
check(len(chr_) == 3, '3 characteristics (device name, 0xfff1, 0xfff2), got %d' % len(chr_))
def vh(uuid):
    for l in chr_:
        if uuid in l.lower(): return int(l.split()[2])
h1, h2 = vh('0xfff1'), vh('0xfff2')
check(h1 == 7 and h2 == 9, 'value handles 7 and 9 (got %s %s)' % (h1, h2))
cccd = [int(l.split()[1]) for l in dsc if '0x2902' in l.lower()]
check(cccd == [10], 'CCCD handle 10 (got %s)' % cccd)
check(p.cmd('ble_read 3', 8)[-1].startswith('read 4e44462d47415454'), 'device name reads "NDF-GATT"')
p.cmd('ble_write %d deadbeef0102' % h1, 8)
r = p.cmd('ble_read %d' % h1, 8)[-1]
check(r == 'read deadbeef0102' + '00' * 10, 'written value reads back (%s)' % r)
try: p.cmd('ble_read 99', 8); check(False, 'read of a missing handle must fail')
except RuntimeError: check(True, 'read of a missing handle -> ATT error')
p.cmd('ble_sub %d' % cccd[0], 8); time.sleep(5)
st = p.cmd('ble_stats')[-1].split()
n, gaps = int(st[1]), int(st[3])
check(n >= 40 and gaps == 0, 'notifications over 5 s: %d (expect ~50), gaps %d' % (n, gaps))
p.cmd('ble_disconnect', 5); time.sleep(2)
check(connect(), 'reconnect after a disconnect')
p.cmd('ble_disc', 30)
check(p.cmd('ble_read %d' % h1, 8)[-1].startswith('read deadbeef'), 'value survives the reconnect')
p.cmd('ble_sub %d' % cccd[0], 8); time.sleep(2)
st = p.cmd('ble_stats')[-1].split()
check(int(st[1]) >= 15, 'notifications again after reconnect (%s)' % st[1])
# abrupt loss: the peer is power-cycled while notifications flow; our side must notice (supervision timeout) and advertise again
ndf.flush(); p.s.write(b'reboot\n'); time.sleep(0.2)
try: ndf.read_until('advertising', 15); lost = True
except TimeoutError: lost = False
check(lost, 'link loss without a disconnect: back to advertising')
time.sleep(6); p.s.reset_input_buffer()
check(connect(), 'central connects again after its own reboot')
p.cmd('ble_disc', 30); p.cmd('ble_sub %d' % cccd[0], 8); time.sleep(2)
check(int(p.cmd('ble_stats')[-1].split()[1]) >= 15, 'notifications after the abrupt loss')
# throughput: 5 s of notifications as fast as the link takes them (0xB5 to 0xfff1)
p.cmd('ble_sub %d' % cccd[0], 8); p.cmd('ble_write %d b5' % h1, 8); time.sleep(6.5)
st = p.cmd('ble_stats')[-1].split(); rate = int(st[1]) / 5.0
print('burst: %s notifications in 5 s = %.0f/s, gaps %s' % (st[1], rate, st[3]))
check(int(st[3]) == 0 and rate >= 739, 'burst without gaps, at least IDF NimBLE server speed (%.0f/s; IDF 739/s on the same 50 ms connection, bench/idf_wl_peer/data/gatts_burst.txt)' % rate)
p.cmd('ble_disconnect', 5)
import subprocess
ndf.close()
if subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), 'test_ble_link.py')]).returncode: fail.append('ble_link (connection update, 2M PHY)')
if subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), 'test_ble_pair.py')]).returncode: fail.append('ble_pair (pairing, encryption, bonds)')
if subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), 'test_ble_privacy.py')]).returncode: fail.append('ble_privacy (resolvable private addresses)')
print('PASS gatts' if not fail else 'FAIL gatts: %s' % fail)
sys.exit(1 if fail else 0)
