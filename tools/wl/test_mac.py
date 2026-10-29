#!/usr/bin/env python3
"""Wi-Fi MAC driver v2 test (components/drivers/rf/test_apps/wifi_mac_unit): the IDF peer runs an access point; the esp-ndf MAC must
count its beacons, answer-check a transmitted probe request, and follow a channel change."""
import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, ap_bssid
from ndf import Ndf

p = Peer(); p.reboot()
n = Ndf(ready='wifi_mac_unit ready', timeout=20)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)

p.cmd('wifi_stop'); BSSID = ap_bssid(p.cmd('wifi_ap T - 6'))
check(n.cmd('init')[-1] == 'init 0', 'wifi_mac_init')
c = n.cmd('count 10000 ' + BSSID, 20)[-1].split()
check(97 <= int(c[2]) <= 102, '10 s of beacons at 100 ms: 97..102, got %s (noise frames dropped: %s)' % (c[2], c[6]))
r = n.cmd('probe %s 200' % BSSID, 10)[-1].split()
check(r[2] == '0' and int(r[4]) >= 1, 'probe request is accepted by the AP and answered (tx %s, responses %s)' % (r[2], r[4]))
for ch in (1, 11, 13, 6):
    p.cmd('wifi_stop'); p.cmd('wifi_ap T - %d 0' % ch); time.sleep(1)
    check(n.cmd('chan %d' % ch)[-1] == 'chan 0', 'channel %d set' % ch)
    c = n.cmd('count 1500 ' + BSSID, 10)[-1].split()
    check(int(c[2]) >= 12, 'channel %d: beacons in 1.5 s: %s (expect ~15)' % (ch, c[2]))
print('FAIL: %s' % fail if fail else 'wifi_mac: all passed'); sys.exit(1 if fail else 0)
