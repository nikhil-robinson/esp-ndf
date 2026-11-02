#!/usr/bin/env python3
"""Exchange memory and core registers while the IDF central on the BOX-3 is INITIATING (connecting to an address nobody advertises): the initiator control structure and the prepared
CONNECT_IND descriptor, as a reference for our own central role. Data: data/em_initiator_<n>.json.  usage: ocd_em_initiator.py [samples]"""
import os, sys, time, json
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'wl')); sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'wifi_regs'))
from ocd import Ocd
from peer import Peer, _board
N = int(sys.argv[1]) if len(sys.argv) > 1 else 3
cen = Peer(_board('BOX3', '')); cen.reboot()
cen.s.write(b'ble_connect 11:22:33:44:55:66 1\n'); time.sleep(2)
with Ocd('30:30:F9:5A:C1:10') as o:
    for n in range(N):
        out = {'map': [], 'em': {}}
        o.cmd('halt'); out['core'] = o.mdw(0x60031000, 0x100)
        for i in range(56):
            v = o.mdw(0x60031204 + 4 * i + (28 if i > 47 else 0), 1)[0]; out['map'].append(v)
            if v & 0x3FFFF:
                off, dram = (v >> 18) << 2, 0x3FC00000 | (v & 0x3FFFF) << 2
                out['em']['%04x' % off] = o.mdw(dram, 256)
        o.cmd('resume'); json.dump(out, open(os.path.join(os.path.dirname(__file__), 'data', 'em_initiator_%d.json' % n), 'w')); time.sleep(0.5)
