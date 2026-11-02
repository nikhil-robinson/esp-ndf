#!/usr/bin/env python3
"""Exchange memory and core registers of the IDF central (PEER board) during a 244-byte notification burst from SERVER=ndf (esp-ndf on the DEVKIT) or SERVER=idf (IDF NimBLE on the BOX-3):
the master CS / exchange table the vendor programs per peer, to find why the central ends events after ~32 ms with our server. Dumps in data/em_central_<server>_<n>.json.  usage: [CAP=server] SERVER=ndf|idf ocd_em_central.py [samples]   (CAP=server dumps the server board instead: DEVKIT for ndf, BOX-3 for idf; files em_server_*)"""
import os, sys, time, json, re
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'wl')); sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'wifi_regs'))
from ocd import Ocd
from peer import Peer, _board
from ndf import Ndf
SERVER = os.environ.get('SERVER', 'ndf'); N = int(sys.argv[1]) if len(sys.argv) > 1 else 3
c = Peer()
if SERVER == 'ndf':
    srv = Ndf(ready='advertise rc 0', timeout=20); time.sleep(5); addr, typ = 'c6:55:44:33:22:11', 1; vh, ch = 7, 10
else:
    srv = Peer(_board('BOX3', '')); srv.reboot(); c.reboot(); srv.cmd('ble_adv BOXSRV'); time.sleep(1)
    m = re.search(r'adv ([0-9a-f:]+) type (\d+)', ' '.join(c.cmd('ble_scan 3 BOXSRV', 10))); addr, typ = m.group(1), int(m.group(2)); vh = ch = None
c.cmd('ble_adv_stop', 5); c.cmd('ble_connect %s %d' % (addr, typ), 20)
d = c.cmd('ble_disc', 30)
if vh is None:
    chrs = [l.split() for l in d if l.startswith('chr')]; vh = int([x for x in chrs if '0xfff1' in x[3].lower()][0][2])
    k = [i for i, l in enumerate(d) if l.startswith('chr') and '0xfff2' in l.lower()][0]
    ch = int([l.split()[1] for l in d[k:] if l.startswith('dsc') and '0x2902' in l.lower()][0])
c.cmd('ble_dle 251 2120', 8); time.sleep(1.5); c.cmd('ble_mtu 247', 8); time.sleep(1)
if SERVER == 'idf': srv.cmd('ble_notify_len 244')
c.cmd('ble_sub %d' % ch, 8)
c.cmd('ble_write %d %s' % (vh, 'b6' if SERVER == 'ndf' else 'b5'), 8); time.sleep(1.5)
CAP = os.environ.get('CAP', 'central')
with Ocd(('24:58:7C:E2:C9:D0' if SERVER == 'ndf' else '30:30:F9:5A:C1:10') if CAP == 'server' else '24:58:7C:E2:C9:48') as o:
    for n in range(N):
        out = {'map': [], 'em': {}}
        o.cmd('halt'); out['core'] = o.mdw(0x60031000, 0x100)
        for i in range(56):
            v = o.mdw(0x60031204 + 4 * i + (28 if i > 47 else 0), 1)[0]; out['map'].append(v)
            if v & 0x3FFFF:
                off, dram = (v >> 18) << 2, 0x3FC00000 | (v & 0x3FFFF) << 2
                out['em']['%04x' % off] = o.mdw(dram, 256)
        o.cmd('resume'); json.dump(out, open(os.path.join(os.path.dirname(__file__), 'data', 'em_%s_%s_%d.json' % (CAP, SERVER, n)), 'w')); time.sleep(0.4)
