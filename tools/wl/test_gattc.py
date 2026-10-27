#!/usr/bin/env python3
"""GATT client test: the IDF peer connects to the esp-ndf device as central and hosts a GATT server (service 0xFFF0); the esp-ndf
app (examples/bluetooth/ble_gattc) uses the same link as a client and prints "gattc PASS" / "gattc FAIL ...". We only drive the peer."""
import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf

ndf = Ndf(ready='advertise rc 0', timeout=20)
p = Peer(); time.sleep(5)
p.cmd('ble_gatts_start', 8); p.cmd('ble_adv_stop', 5)
p.cmd('ble_connect c6:55:44:33:22:11 1', 20)
lines = []
try:
    while True:
        l = ndf.read_until('gattc ', 60)[-1]
        lines.append(l); print(l)
        if l.startswith('gattc PASS') or l.startswith('gattc FAIL'): break
except TimeoutError as e:
    print('FAIL no result from the esp-ndf client:', e); sys.exit(1)
p.cmd('ble_disconnect', 5)
sys.exit(0 if lines[-1].startswith('gattc PASS') else 1)
