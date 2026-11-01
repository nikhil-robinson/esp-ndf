#!/usr/bin/env python3
"""IDF station on the BOX-3 pings esp-ndf ip_unit on the DEVKIT through the IDF AP (PEER), 2 x 200, with our MAC / lwIP counters.
usage: probe_relay_ping.py [ap channel args, default 6]   Data: bench/idf_wl_peer/data/relay_ping_devkit.txt"""
import sys, time
sys.path.insert(0, 'tools/wl')
from ndf import Ndf
from peer import Peer, _board
p = Peer(); box = Peer(_board('BOX3', '')); p.reboot(); box.reboot(); time.sleep(3)
p.cmd('wifi_stop'); p.cmd('wifi_ap T - ' + (sys.argv[1] if len(sys.argv) > 1 else '6')); time.sleep(1)
n = Ndf(port=_board('DEVKIT', ''), ready='ip_unit ready'); print(n.cmd('up T', 40)[-1])
box.cmd('wifi_stop'); print(box.cmd('wifi_sta T -', 40)[-1]); time.sleep(2)
for _ in range(2):
    a = n.cmd('mac')[-1] + ' | ' + n.cmd('stats')[-1]; print(box.cmd('ping 192.168.4.2 200', 300)[-1]); print('  ', a); print('  ', n.cmd('mac')[-1] + ' | ' + n.cmd('stats')[-1], flush=True)
box.cmd('wifi_stop')
