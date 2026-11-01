#!/usr/bin/env python3
"""BLE pairing and link encryption against the IDF central: LE Secure Connections, Just Works, bonding. The central pairs with the esp-ndf server (examples/bluetooth/ble_gatts),
the link is encrypted, notifications keep flowing; after a reconnect the stored key encrypts the link again without a new pairing; a central that lost its bond pairs anew."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf
ADDR = 'c6:55:44:33:22:11'
ndf = Ndf(ready='advertise rc 0', timeout=20)
p = Peer(); time.sleep(5)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)
def info():
    l = p.cmd('ble_info')[-1]; return {k: int(v) for k, v in re.findall(r'(\w+) (-?\d+)', l)}
def stats():
    s = p.cmd('ble_stats')[-1].split(); return int(s[1]), int(s[3])
def connect():
    p.cmd('ble_adv_stop', 5); p.cmd('ble_connect %s 1' % ADDR, 20); p.cmd('ble_disc', 30); p.cmd('ble_sub 10', 8)
p.cmd('ble_unbond'); connect()
check(info()['enc'] == 0, 'link starts unencrypted')
t0 = time.time()
try: r = p.cmd('ble_pair', 20); ok = True
except Exception as e: print(e); ok = False
check(ok, 'pairing (LE Secure Connections, Just Works) completes in %.1f s' % (time.time() - t0))
i = info(); print(i)
check(i['enc'] == 1 and i['bonded'] == 1 and i['keysize'] == 16, 'link encrypted, bonded, 16-byte key')
n0, g0 = stats(); time.sleep(3); n1, g1 = stats()
check(n1 - n0 >= 20 and g1 == 0, 'notifications flow on the encrypted link (%d in 3 s, gaps %d)' % (n1 - n0, g1))
p.cmd('ble_write 7 deadbeef0102', 8); rd = p.cmd('ble_read 7', 8)[-1]
check(rd.startswith('read deadbeef0102'), 'encrypted write and read (%s)' % rd[:24])
p.cmd('ble_disconnect', 5); time.sleep(2)
connect()
check(info()['enc'] == 0, 'a new link starts unencrypted')
t0 = time.time()
try: r = p.cmd('ble_pair', 20); ok = True
except Exception as e: print(e); ok = False
check(ok and info()['enc'] == 1, 'bonded: the stored key encrypts the link again (%.1f s, no new pairing)' % (time.time() - t0))
n0, g0 = stats(); time.sleep(3); n1, g1 = stats()
check(n1 - n0 >= 20 and g1 == 0, 'notifications after re-encryption (%d in 3 s)' % (n1 - n0))
p.cmd('ble_disconnect', 5); time.sleep(2)
# a new boot of the esp-ndf board: the bond comes back from NVS
ndf.close(); ndf = Ndf(ready='advertise rc 0', timeout=20); time.sleep(1)
connect()
t0 = time.time()
try: p.cmd('ble_pair', 20); ok = True
except Exception as e: print(e); ok = False
check(ok and info()['enc'] == 1 and info()['bonded'] == 1, 'after a reboot of the server the bond is still there (NVS): encrypted in %.1f s' % (time.time() - t0))
p.cmd('ble_disconnect', 5); time.sleep(2)
p.cmd('ble_unbond'); connect()
try: p.cmd('ble_pair', 20); ok = True
except Exception as e: print(e); ok = False
check(ok and info()['enc'] == 1, 'the central lost its bond: pairs anew with our old bond in place')
p.cmd('ble_disconnect', 5)
print('PASS ble_pair' if not fail else 'FAIL ble_pair: %s' % fail)
sys.exit(1 if fail else 0)
