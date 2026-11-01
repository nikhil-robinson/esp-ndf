#!/usr/bin/env python3
"""Exchange memory of the BLE core while a server sends a 5 s burst of big notifications to the IDF central: SERVER=idf (BOX-3) or ndf (DEVKIT). JTAG halt mid-burst, the page map
(0x60031204..) resolves every EM page to DRAM, which is read and saved as data/em_burst_<server>.json {em_offset: [words]} with the map entries.  usage: ocd_em_burst.py"""
import os, sys, time, json, re
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'wl')); sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'wifi_regs'))
from ocd import Ocd
from peer import Peer, _board
from ndf import Ndf
SERVER = os.environ.get('SERVER', 'ndf'); c = Peer()
if SERVER == 'ndf':
    srv = Ndf(ready='advertise rc 0', timeout=20); time.sleep(5); addr, typ, vh, ch, usb = 'c6:55:44:33:22:11', 1, 7, 10, '24:58:7C:E2:C9:D0'
else:
    srv = Peer(_board('BOX3', '')); srv.reboot(); c.reboot(); srv.cmd('ble_adv BOXSRV'); time.sleep(1)
    m = re.search(r'adv ([0-9a-f:]+) type (\d+)', ' '.join(c.cmd('ble_scan 3 BOXSRV', 10))); addr, typ = m.group(1), int(m.group(2)); usb = '30:30:F9:5A:C1:10'; vh = ch = None
c.cmd('ble_adv_stop', 5); c.cmd('ble_connect %s %d' % (addr, typ), 20); d = c.cmd('ble_disc', 30)
if vh is None:
    chrs = [l.split() for l in d if l.startswith('chr')]; vh = int([x for x in chrs if '0xfff1' in x[3].lower()][0][2])
    k = [i for i, l in enumerate(d) if l.startswith('chr') and '0xfff2' in l.lower()][0]; ch = int([l.split()[1] for l in d[k:] if l.startswith('dsc') and '0x2902' in l.lower()][0])
c.cmd('ble_dle 251 2120', 8); time.sleep(1.5); c.cmd('ble_mtu 247', 8); time.sleep(1)
if SERVER == 'idf': srv.cmd('ble_notify_len 244')
c.cmd('ble_sub %d' % ch, 8); c.s.write(('ble_write %d %s\n' % (vh, 'b6' if SERVER == 'ndf' else 'b5')).encode()); time.sleep(2.5)
out = {'map': [], 'em': {}}
with Ocd(usb) as o:
    o.cmd('halt')
    for i in range(56):
        v = o.mdw(0x60031204 + 4 * i + (28 if i > 47 else 0), 1)[0]; out['map'].append(v)
        if v & 0x3FFFF and (v >> 18):
            off, dram = (v >> 18) << 2, 0x3FC00000 | (v & 0x3FFFF) << 2
            out['em']['%04x' % off] = o.mdw(dram, 256)
    o.cmd('resume')
json.dump(out, open(os.path.join(os.path.dirname(__file__), 'data', 'em_burst_%s.json' % SERVER), 'w'))
time.sleep(5); print(c.cmd('ble_stats')[-1]); c.cmd('ble_disconnect', 5)
