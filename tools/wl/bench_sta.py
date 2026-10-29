#!/usr/bin/env python3
"""Station comparison on the same air (docs/PERFORMANCE.md): 64 KB TCP echo and 300 UDP datagrams through the IDF AP (PEER), esp-ndf station (DEVKIT, ip_unit)
versus an ESP-IDF station (BOX-3, esp_wifi + lwIP sockets). Open and WPA2. usage: bench_sta.py [rounds]"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
R = int(sys.argv[1]) if len(sys.argv) > 1 else 3
RATE = os.environ.get('AP_RATE', '0')                        # the AP's fixed PHY rate code for both stations (0 = 1 Mbit/s, 3 = 11 Mbit/s CCK)
p = Peer(); box = Peer(_board('BOX3', ''))
for pw in ('-', 'secret123'):
    p.reboot(); box.reboot(); p.cmd('wifi_stop'); box.cmd('wifi_stop'); p.cmd('wifi_ap T %s 6 %s' % (pw, RATE)); time.sleep(1); p.cmd('tcp_echo 8')
    n = Ndf(ready='ip_unit ready', timeout=20); n.cmd('up T' + ('' if pw == '-' else ' ' + pw), 40)
    ndf = [int(re.search(r'in (\d+) ms', n.cmd('tcp 192.168.4.1 8 65536', 300)[-1]).group(1)) for _ in range(R)]
    n.close(); time.sleep(1)
    n2 = Ndf(ready='ip_unit ready', timeout=20); n2.close()            # resets the DEVKIT: our station leaves the air
    box.cmd('wifi_sta T %s' % pw, 40); idf = []
    for _ in range(R):
        r = box.cmd('tcp_send 192.168.4.1 8 65536', 300)[-1]; assert 'echoed=65536 bad=0' in r, r; idf.append(int(re.search(r'elapsed_ms=(\d+)', r).group(1)))   # on the device, after connect: the same window ip_unit measures
    ndf.sort(); idf.sort(); print('%-6s rate %s TCP echo 64 KB  esp-ndf median %d ms %s  ESP-IDF median %d ms %s' % ('open' if pw == '-' else 'WPA2', RATE, ndf[len(ndf) // 2], ndf, idf[len(idf) // 2], idf), flush=True)
