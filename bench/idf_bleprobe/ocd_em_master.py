#!/usr/bin/env python3
"""Exchange memory and core registers of the BLE core while the IDF NimBLE stack on the BOX-3 is the CENTRAL of an idle connection to the IDF peripheral on the PEER board:
the master control structure / exchange table / TX descriptor the vendor uses, as a reference for our own central role. Several halts, dumps in data/em_master_<n>.json.
usage: ocd_em_master.py [samples]"""
import os, sys, time, json, re
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'wl')); sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'wifi_regs'))
from ocd import Ocd
from peer import Peer, _board
N = int(sys.argv[1]) if len(sys.argv) > 1 else 3
adv = Peer(); cen = Peer(_board('BOX3', '')); adv.reboot(); cen.reboot(); adv.cmd('ble_adv PEERADV'); time.sleep(1)
m = re.search(r'adv ([0-9a-f:]+) type \d+ .* atype (\d+)', ' '.join(cen.cmd('ble_scan 3 PEERADV', 15))); addr, typ = m.group(1), int(m.group(2))
cen.cmd('ble_connect %s %d' % (addr, typ), 20); time.sleep(2)
with Ocd('30:30:F9:5A:C1:10') as o:
    for n in range(N):
        out = {'map': [], 'em': {}}
        o.cmd('halt'); out['core'] = o.mdw(0x60031000, 0x100)
        for i in range(56):
            v = o.mdw(0x60031204 + 4 * i + (28 if i > 47 else 0), 1)[0]; out['map'].append(v)
            if v & 0x3FFFF:
                off, dram = (v >> 18) << 2, 0x3FC00000 | (v & 0x3FFFF) << 2
                out['em']['%04x' % off] = o.mdw(dram, 256)
        o.cmd('resume'); json.dump(out, open(os.path.join(os.path.dirname(__file__), 'data', 'em_master_%d.json' % n), 'w')); time.sleep(0.7)
print(cen.cmd('ble_info')[-1]); cen.cmd('ble_disconnect', 5)
