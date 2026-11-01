#!/usr/bin/env python3
"""BLE privacy against the IDF central: after bonding the server advertises with resolvable private addresses (hash(IRK, prand) | prand, top bits 01) that change every 5 s;
the test resolves them with the IRK the server printed, then connects to one of them and encrypts the link with the stored bond. Needs examples/bluetooth/ble_gatts."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
ADDR = 'c6:55:44:33:22:11'
ndf = Ndf(ready='advertise rc 0', timeout=20)
p = Peer(); time.sleep(5)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)
def info():
    l = p.cmd('ble_info')[-1]; return {k: int(v) for k, v in re.findall(r'(\w+) (-?\d+)', l)}
def ah(irk_le, prand_msb):                                   # Core spec vol 3 part H 2.2.2, most significant octet first
    k = bytes(reversed(irk_le)); e = Cipher(algorithms.AES(k), modes.ECB()).encryptor().update(bytes(13) + prand_msb)
    return e[13:16]
def scan():
    out = p.cmd('ble_scan 4 NDF-GATT', 15); m = re.search(r'adv ([0-9a-f:]+) type \d+ .* atype (\d+)', ' '.join(out)); return (m.group(1), int(m.group(2))) if m else (None, None)
def rpa_ok(addr, typ, irk):
    b = bytes(int(x, 16) for x in addr.split(':'))           # printed most significant first: prand (3) | hash (3)
    return typ == 1 and (b[0] & 0xC0) == 0x40 and ah(irk, b[0:3]) == b[3:6]
p.cmd('ble_unbond'); p.cmd('ble_adv_stop', 5); p.cmd('ble_connect %s 1' % ADDR, 20); p.cmd('ble_disc', 30)
p.cmd('ble_pair', 20); check(info()['bonded'] == 1, 'bonded with the server')
ndf.flush(); p.cmd('ble_write 7 b7', 8); time.sleep(1)
irk = None
for l in ndf.read_until('privacy on', 5):
    m = re.search(r'privacy on, irk((?: [0-9a-f]{2}){16})', l)
    if m: irk = bytes(int(x, 16) for x in m.group(1).split())
check(irk is not None, 'the server switched privacy on')
p.cmd('ble_disconnect', 5); time.sleep(2)
ar, tr = scan(); print('seen by the bonded central:', ar, tr)
check(ar == ADDR, 'the bonded central resolves the private address to our identity address (%s)' % ar)
p.reboot(); time.sleep(2)                                                      # a fresh central without our IRK shows what is on the air
a1, t1 = scan(); print('advertised', a1, t1)
check(a1 is not None and rpa_ok(a1, t1, irk), 'advertising address %s is a resolvable private address of the server\'s IRK' % a1)
time.sleep(6)
a2, t2 = scan(); print('advertised', a2, t2)
check(a2 is not None and a2 != a1 and rpa_ok(a2, t2, irk), 'the address changed after the rotation time: %s' % a2)
p.cmd('ble_adv_stop', 5)
try: p.cmd('ble_connect %s 1' % a2, 20); ok = True
except Exception as e: print(e); ok = False
check(ok, 'the central connects to the private address')
if ok:
    p.cmd('ble_disc', 30)
    try: p.cmd('ble_pair', 20); ok = True
    except Exception as e: print(e); ok = False
    i = info(); print(i)
    check(ok and i['enc'] == 1, 'the link is encrypted over the private address')
    p.cmd('ble_disconnect', 5)
ndf.flush(); time.sleep(1)
print('PASS ble_privacy' if not fail else 'FAIL ble_privacy: %s' % fail)
sys.exit(1 if fail else 0)
