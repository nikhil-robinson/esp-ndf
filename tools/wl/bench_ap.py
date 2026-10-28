#!/usr/bin/env python3
"""Access point comparison on the same air (docs/PERFORMANCE.md): beacon interval jitter and the time a station needs to join (association + DHCP),
esp-ndf AP versus an ESP-IDF softAP. Boards: DEVKIT (esp-ndf AP), PEER (IDF station / IDF softAP), BOX-3 (sniffer; IDF softAP for the IDF run is the PEER, the station the BOX-3).
usage: bench_ap.py [WPA2-passphrase]"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board, ap_bssid
from ndf import Ndf
pw = sys.argv[1] if len(sys.argv) > 1 else None
box = Peer(_board('BOX3', '')); peer = Peer(); time.sleep(4)
res = {}
def bcn_stats(mon, bssid, ms=8000):
    mon.cmd('wifi_stop'); mon.cmd('wifi_mon 6'); l = mon.cmd('wifi_bcn %s 64 %d' % (bssid, ms), ms // 1000 + 10)[-1]
    m = re.search(r'heard (\d+) intervals (\d+) mean_us (\d+) min_us (\d+) max_us (\d+) dev_us (\d+)', l); return [int(v) for v in m.groups()]

# esp-ndf AP, IDF station on the PEER, sniffer BOX-3
peer.cmd('wifi_stop'); box.cmd('wifi_stop')
n = Ndf(ready='ap_unit ready', timeout=20)
n.cmd('start ndf-ap' + (' ' + pw if pw else ''), 10)
res['ndf beacons (heard, intervals, mean, min, max, dev us)'] = bcn_stats(box, '02:4e:44:46:00:01')
joins = []
for i in range(5):
    peer.cmd('wifi_stop'); n.cmd('stop'); n.cmd('start ndf-ap' + (' ' + pw if pw else ''), 10); time.sleep(1)
    t = time.time(); peer.cmd('wifi_sta ndf-ap %s' % (pw or '-'), 40); joins.append(time.time() - t)
res['ndf AP: station join+DHCP s'] = joins
n.cmd('stop'); peer.cmd('wifi_stop')

# IDF softAP on the PEER, IDF station on the BOX-3 (wifi_sta), sniffer cannot be the same board: beacons are measured from the BOX-3 before it joins
bssid = ap_bssid(peer.cmd('wifi_ap idf-ap %s 6 0' % (pw or '-'))); time.sleep(1)
res['idf beacons (heard, intervals, mean, min, max, dev us)'] = bcn_stats(box, bssid)
joins = []
for i in range(5):
    box.cmd('wifi_stop'); peer.cmd('wifi_stop'); peer.cmd('wifi_ap idf-ap %s 6 0' % (pw or '-')); time.sleep(1)
    t = time.time(); box.cmd('wifi_sta idf-ap %s' % (pw or '-'), 40); joins.append(time.time() - t)
res['idf softAP: station join+DHCP s'] = joins
for k, v in res.items(): print(k, ['%.2f' % x for x in v] if isinstance(v[0], float) else v)
