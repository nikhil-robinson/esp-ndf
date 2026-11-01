#!/usr/bin/env python3
"""Does our station's capability change the IDF station's own link? The BOX-3 (IDF station) runs 64 KB TCP echoes to the IDF AP while our
station (ip_unit, DEVKIT) only sits joined in the same BSS. Run once per build to compare (e.g. NDF_WIFI_HT40 1 vs 0). usage: probe_bss_leg.py [n] [ap args]"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
N = int(sys.argv[1]) if len(sys.argv) > 1 else 8; AP = sys.argv[2] if len(sys.argv) > 2 else 'T - 6'
p = Peer(); box = Peer(_board('BOX3', '')); p.reboot(); box.reboot(); time.sleep(3)
import atexit; atexit.register(lambda: box.cmd('wifi_stop'))
p.cmd('wifi_stop'); p.cmd('wifi_ap ' + AP); time.sleep(1); p.cmd('tcp_echo 8')
n = Ndf(ready='ip_unit ready', timeout=20); print(n.cmd('up T', 40)[-1])
box.cmd('wifi_stop'); box.cmd('wifi_sta T -', 40); tt = []
for _ in range(N):
    try: r = box.cmd('tcp_send 192.168.4.1 8 65536', 90)[-1]; tt.append(int(re.search(r'elapsed_ms=(\d+)', r).group(1)) if 'bad=0' in r else -1)
    except Exception as e: tt.append(-1)
print('IDF station leg, ours joined:', sorted(tt), flush=True)
