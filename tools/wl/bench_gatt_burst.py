#!/usr/bin/env python3
"""GATT notification throughput, server = esp-ndf (examples/bluetooth/ble_gatts on the DEVKIT) or the IDF NimBLE server (SERVER=idf, on the BOX-3), client = the IDF central on the
PEER board. A 5 s burst of notifications of LEN bytes, with data length 251 / ATT MTU 247 (DLE=1) and/or the 2M PHY (PHY=2).  env: SERVER ndf|idf, LEN (default 244), DLE (1), PHY (1|2)
Prints notifications/s, kbit/s of payload, gaps."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
SERVER, LEN, DLE, PHY = os.environ.get('SERVER', 'ndf'), int(os.environ.get('LEN', 244)), os.environ.get('DLE', '1') == '1', int(os.environ.get('PHY', 1))
c = Peer()
if SERVER == 'ndf':
    srv = Ndf(ready='advertise rc 0', timeout=20); time.sleep(5); addr, typ = 'c6:55:44:33:22:11', 1; vh, ch = 7, 10
else:
    srv = Peer(_board('BOX3', '')); srv.reboot(); c.reboot(); srv.cmd('ble_adv BOXSRV'); time.sleep(1)
    out = c.cmd('ble_scan 3 BOXSRV', 10); m = re.search(r'adv ([0-9a-f:]+) type (\d+)', ' '.join(out)); assert m, out
    addr, typ = m.group(1), int(m.group(2)); vh = ch = None
c.cmd('ble_adv_stop', 5); c.cmd('ble_connect %s %d' % (addr, typ), 20)
d = c.cmd('ble_disc', 30)
if vh is None:
    chrs = [l.split() for l in d if l.startswith('chr')]; vh = int([x for x in chrs if '0xfff1' in x[3].lower()][0][2])
    k = [i for i, l in enumerate(d) if l.startswith('chr') and '0xfff2' in l.lower()][0]
    ch = int([l.split()[1] for l in d[k:] if l.startswith('dsc') and '0x2902' in l.lower()][0])       # the CCCD that follows the 0xfff2 characteristic
if DLE: c.cmd('ble_dle 251 2120', 8); time.sleep(1.5); c.cmd('ble_mtu 247', 8); time.sleep(1)
if PHY == 2: c.cmd('ble_phy 2', 8); time.sleep(3)
if SERVER == 'idf': srv.cmd('ble_notify_len %d' % LEN)
c.cmd('ble_sub %d' % ch, 8)
c.cmd('ble_write %d %s' % (vh, 'b6' if SERVER == 'ndf' and LEN > 4 else 'b5'), 8); time.sleep(6.5)
s = c.cmd('ble_stats')[-1].split(); n, g, b = int(s[1]), int(s[3]), int(s[-1]) if 'bytes' in s else 0
print('%s LEN %d DLE %d PHY %dM: %d notifications in 5 s = %d/s, %d kbit/s payload, gaps %d  | %s' % (SERVER, LEN, DLE, PHY, n, n // 5, b * 8 // 5000, g, ' '.join(c.cmd('ble_info'))))
c.cmd('ble_disconnect', 5)
