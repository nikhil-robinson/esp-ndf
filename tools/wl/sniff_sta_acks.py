#!/usr/bin/env python3
"""Third-board view of our station's transmissions: the IDF AP (PEER) pings esp-ndf ip_unit on the DEVKIT 300 times while the BOX-3 (IDF peer,
wifi_sniffta) counts the station's data frames (first tries / retries) and the ACKs addressed to it. usage: sniff_sta_acks.py <ap args> [mac]
Data: bench/idf_wl_peer/data/sniff_sta_acks_devkit.txt"""
import os, sys, time, threading
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
ap = sys.argv[1] if len(sys.argv) > 1 else '6 - above'; mac = sys.argv[2] if len(sys.argv) > 2 else '02:4e:44:46:00:02'
p = Peer(); sn = Peer(_board('BOX3', '')); p.reboot(); sn.reboot(); time.sleep(3)
p.cmd('wifi_stop'); p.cmd('wifi_ap T - ' + ap); time.sleep(1)
idf = os.environ.get('IDF') == '1'                          # IDF=1: the IDF peer as the station on the DEVKIT (its MAC as the argument)
if idf: n = Peer(_board('DEVKIT', '')); n.reboot(); time.sleep(2); n.cmd('wifi_stop'); print(n.cmd('wifi_sta T -', 40)[-1])
else: n = Ndf(port=_board('DEVKIT', ''), ready='ip_unit ready'); print(n.cmd('up T', 40)[-1])
if os.environ.get('RATE'): n.cmd('rate ' + os.environ['RATE'])
time.sleep(2); a = '' if idf else n.cmd('mac')[-1]; out = {}
t = threading.Thread(target=lambda: out.update(s=sn.cmd('wifi_sniffta %s 24 %s' % (ap.split()[0], mac), 40)[-1])); t.start(); time.sleep(2)
print(p.cmd('ping 192.168.4.2 300', 400)[-1]); t.join()
print(out.get('s'))
if idf: n.cmd('wifi_stop')
else: print(' before', a); print(' after ', n.cmd('mac')[-1])
