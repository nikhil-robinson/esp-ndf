#!/usr/bin/env python3
"""RSSI the IDF AP (PEER) reports for a station on the DEVKIT: phase 'idf' (the peer image flashed to the DEVKIT, tools/wl/flash_peer.sh devkit) or
phase 'ndf' (ip_unit on the DEVKIT). Same board, same place, so the difference is our transmit power. usage: rssi_same_board.py idf|ndf [samples]"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
ph = sys.argv[1]; N = int(sys.argv[2]) if len(sys.argv) > 2 else 10
p = Peer(); p.reboot(); p.cmd('wifi_stop'); p.cmd('wifi_ap T - 6'); time.sleep(1)
if ph == 'idf':
    d = Peer(_board('DEVKIT', '')); d.reboot(); d.cmd('wifi_stop'); d.cmd('wifi_sta T -', 40)
    def traffic(): d.cmd('ping 192.168.4.1 5', 30)
else:
    n = Ndf(ready='ip_unit ready', timeout=20); assert n.cmd('up T', 40)[-1].startswith('up rc 0')
    def traffic(): n.cmd('ping 192.168.4.1 5', 30)
v = []
for _ in range(N):
    traffic(); v += [int(x) for x in re.findall(r'rssi (-?\d+)', '\n'.join(p.cmd('wifi_stas')))]
if ph == 'idf': d.cmd('wifi_stop')
v.sort(); print('%s rssi median %d %s' % (ph, v[len(v) // 2], v))
