#!/usr/bin/env python3
"""IDF client on the BOX-3 -> AP -> our WebSocket server (http_unit, DEVKIT): MAC counters before and after one session of 20 echoes, repeated.
usage: probe_ws_relay.py [rounds] [ap args]   (diagnostic for the test_ws relay case)"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
R = int(sys.argv[1]) if len(sys.argv) > 1 else 3; AP = sys.argv[2] if len(sys.argv) > 2 else 'T - 6'
p = Peer(); box = Peer(_board('BOX3', '')); p.reboot(); box.reboot(); time.sleep(3)
import atexit; atexit.register(lambda: box.cmd('wifi_stop'))
p.cmd('wifi_stop'); p.cmd('wifi_ap ' + AP); time.sleep(1)
n = Ndf(ready='http_unit ready', timeout=20); print(n.cmd('up T', 40)[-1]); n.cmd('serve 80'); n.cmd('wsserve')
box.cmd('wifi_stop'); box.cmd('wifi_sta T -', 40)
for _ in range(R):
    a = n.cmd('mac')[-1]; g = box.cmd('ws_get ws://192.168.4.2/ws 20 1000', 120)[-1]; b = n.cmd('mac')[-1]
    print(g); print('  before', a); print('  after ', b, flush=True)
