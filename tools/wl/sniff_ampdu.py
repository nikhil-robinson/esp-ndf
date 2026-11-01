#!/usr/bin/env python3
"""Third-board view of our A-MPDU transmit: IDF AP on the PEER board (channel 1), esp-ndf ip_unit (-DNDF_WIFI_AMPDU_TX=1) on the BOX-3 sends a TCP upload, the DEVKIT
(IDF peer, wifi_sniffta) counts the station's data frames (first tries / retries) and the ACKs addressed to it. usage: sniff_ampdu.py"""
import os, sys, time, threading
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
mac = '02:4e:44:46:00:02'
p = Peer(); sn = Peer(_board('DEVKIT', '')); p.reboot(); sn.reboot(); time.sleep(3)
p.cmd('wifi_stop'); p.cmd('wifi_ap T - 1 -'); time.sleep(1); p.cmd('tcp_echo 8')
n = Ndf(port=_board('BOX3', ''), ready='ip_unit ready'); assert 'rc 0' in n.cmd('up T', 40)[-1]
n.cmd('rate 0x17'); n.cmd('ping 192.168.4.1 5', 30); time.sleep(3); n.cmd('ping 192.168.4.1 3', 30)
out = {}
t = threading.Thread(target=lambda: out.update(s=sn.cmd('wifi_sniffta 1 20 %s' % mac, 40)[-1])); t.start(); time.sleep(2)
print(n.cmd('tcp 192.168.4.1 8 30000', 60)[-1]); t.join()
print(out.get('s')); print(" ".join(n.cmd("mac")))
