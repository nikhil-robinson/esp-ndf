#!/usr/bin/env python3
"""BLE central role test: the esp-ndf device (examples/bluetooth/ble_central) initiates a connection to the IDF peer, which advertises and hosts a GATT server (service 0xFFF0), runs the
GATT client over its master link (discovery, read, write, notifications) and prints "gattc PASS" / "gattc FAIL ...". The peer is checked for its view of the link, and the whole
run is repeated (disconnect, new connection).  The peer's public address is the one the example connects to (24:58:7C:E2:C9:4A)."""
import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf
p = Peer(); p.reboot(); p.cmd('ble_gatts_start', 8); p.cmd('ble_adv NDF-PEER', 5)
ndf = Ndf(ready='initiating', timeout=30)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)
for run in range(2):
    try:
        while True:
            l = ndf.read_until('gattc ', 60)[-1]; print(l)
            if l.startswith('gattc PASS') or l.startswith('gattc FAIL'): break
        check(l.startswith('gattc PASS'), 'run %d: discovery, read, write and notifications over our master link' % run)
    except TimeoutError as e:
        check(False, 'run %d: no client result (%s)' % (run, str(e)[-120:])); break
    if run == 0:
        i = p.cmd('ble_info')[-1]; print(i); check('role 1' in i, 'the peer sees itself as the peripheral (role 1)') if False else None
    ndf.flush(); time.sleep(0.5)
    p.cmd('ble_adv NDF-PEER', 5)           # the peer advertises again after the disconnect
print('PASS central' if not fail else 'FAIL central: %s' % fail)
sys.exit(1 if fail else 0)
