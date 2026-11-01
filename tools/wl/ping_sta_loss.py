#!/usr/bin/env python3
"""AP -> station frame loss: the IDF AP (PEER) pings the station 300 times (50 ms apart), "<ap args>" e.g. "6 - above".
usage: ping_sta_loss.py idf|ndf <ap args>...   station on the DEVKIT (STA_PORT to change); idf = bench/idf_wl_peer, ndf = ip_unit.
Data: bench/idf_wl_peer/data/ping_sta_loss_devkit.txt"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
S = os.environ.get('STA_PORT') or _board('DEVKIT', ''); kind = sys.argv[1]
p = Peer()
for ap in sys.argv[2:]:
    p.reboot(); time.sleep(3); p.cmd('wifi_stop'); p.cmd('wifi_ap T - ' + ap); time.sleep(1)
    if kind == 'idf': s = Peer(S); s.reboot(); time.sleep(2); s.cmd('wifi_stop'); s.cmd('wifi_sta T -', 40)
    else:
        s = Ndf(port=S, ready='ip_unit ready'); print('  up', s.cmd('up T', 40)[-1])
        if os.environ.get('RATE'): s.cmd('rate ' + os.environ['RATE'])     # our data rate code, e.g. 0xb = 6 Mbit/s
    time.sleep(2)
    for _ in range(3):
        m = (lambda: s.cmd('mac')[-1] + ' | ' + s.cmd('stats')[-1]) if kind == 'ndf' else (lambda: '')
        a = m(); print(kind, 'AP "%s"' % ap, p.cmd('ping 192.168.4.2 300', 400)[-1], flush=True)
        if kind == 'ndf': print('   before', a); print('   after ', m(), flush=True)
    if kind == 'idf': s.cmd('wifi_stop')
    s.s.close() if hasattr(s, 's') else s.close()
