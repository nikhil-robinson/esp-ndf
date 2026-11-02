#!/usr/bin/env python3
"""BLE peripheral with two links at once: the esp-ndf GATT server (examples/bluetooth/ble_gatts_multi on the DEVKIT) is connected to by two IDF centrals, PEER and the BOX-3, both with the peer image.
Each discovers the server and subscribes to the counter; both must receive the notifications (10/s) without a gap, the second link must keep running when the first is dropped, and the dropped
central must be able to connect again while the other link is up."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
ADDR = 'c6:55:44:33:22:11'
ndf = Ndf(ready='advertise rc 0', timeout=20)
a = Peer(); b = Peer(_board('BOX3', '')); time.sleep(5)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)
def stats(p):
    s = p.cmd('ble_stats')[-1]; return {k: int(v) for k, v in re.findall(r'(\w+) (-?\d+)', s)}
def connect(p):
    p.cmd('ble_adv_stop', 5); p.cmd('ble_connect %s 1' % ADDR, 20); p.cmd('ble_disc', 30); p.cmd('ble_sub 10', 8)
def flow(p, name, secs=3):
    s0 = stats(p); time.sleep(secs); s1 = stats(p)
    n = s1['notifications'] - s0['notifications']
    check(n >= 8 * secs and s1['gaps'] == s0['gaps'] and s1['connected'] == 1, '%s: notifications flow (%d in %d s, gaps %d)' % (name, n, secs, s1['gaps']))
connect(a); flow(a, 'first link (PEER)')
connect(b); time.sleep(1); flow(b, 'second link (BOX-3)'); flow(a, 'first link still running')
a.cmd('ble_disconnect', 8); time.sleep(2)
flow(b, 'second link after the first dropped')
connect(a); time.sleep(1); flow(a, 'first central reconnected while the second link is up'); flow(b, 'second link unaffected')
print('PASS gatts_multi' if not fail else 'FAIL gatts_multi: %s' % fail)
sys.exit(1 if fail else 0)
