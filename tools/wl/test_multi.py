#!/usr/bin/env python3
"""BLE central with two links at once: the esp-ndf device (examples/bluetooth/ble_multi) connects to two IDF peripherals, PEER (24:58:7C:E2:C9:4A) and the BOX-3 (30:30:F9:5A:C1:12), both with the peer
image and a GATT server (service 0xFFF0, counter notifications 10/s). It must keep both links up, receive the notifications of both without a gap, and keep the second link running when the first
is dropped. The whole run is repeated once (new connections)."""
import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
a = Peer(); b = Peer(_board('BOX3', ''))
for p, n in ((a, 'NDF-PEER-A'), (b, 'NDF-PEER-B')):
    p.reboot(); p.cmd('ble_gatts_start', 8); p.cmd('ble_adv ' + n, 5)
ndf = Ndf(ready='ble_hw_init', timeout=30)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)
def readvertise():                       # a peer that is not connected advertises (it stops when a link ends, and the DEVKIT starts connecting as soon as it is flashed)
    for p, n in ((a, 'NDF-PEER-A'), (b, 'NDF-PEER-B')):
        try:
            if 'connected 0' in p.cmd('ble_stats')[-1]: p.cmd('ble_adv ' + n, 5)
        except RuntimeError: pass
for run in range(2):
    try:
        t0 = time.time()
        while True:
            try: ls = ndf.read_until('multi ', 12); print('\n'.join(ls[-4:])); l = ls[-1]
            except TimeoutError:
                if time.time() - t0 > 150: raise
                readvertise(); continue
            if l.startswith('multi PASS') or l.startswith('multi FAIL'): break         # 'multi: ...' progress lines do not end the run
        check(l.startswith('multi PASS'), 'run %d: two links, notifications on both, the second survives the first' % run)
    except TimeoutError as e:
        check(False, 'run %d: no result (%s)' % (run, str(e)[-160:])); break
    ndf.flush(); time.sleep(0.5)
    a.cmd('ble_adv NDF-PEER-A', 5); b.cmd('ble_adv NDF-PEER-B', 5)      # the peers advertise again after the disconnect
print('PASS multi' if not fail else 'FAIL multi: %s' % fail)
sys.exit(1 if fail else 0)
