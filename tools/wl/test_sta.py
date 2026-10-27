#!/usr/bin/env python3
"""Wi-Fi station test (components/wifi/test_apps/sta_unit): the IDF peer runs an open access point; the esp-ndf station scans, joins,
exchanges ARP through the AP, notices when the AP vanishes, joins again, and ignores a deauthentication that is not from its AP."""
import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf

BSSID = '30:30:f9:5a:c1:11'
p = Peer(); time.sleep(4)
n = Ndf(ready='sta_unit ready', timeout=20)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)

p.cmd('wifi_stop'); p.cmd('wifi_ap T - 6'); p.s.reset_input_buffer()
check(n.cmd('init')[-1] == 'init 0', 'wifi_init')
r = n.cmd('scan', 30)
mine = [l for l in r if l.startswith('ap ') and BSSID in l]
check(len(mine) == 1 and ' ch 6 ' in mine[0] and ' sec 0 ' in mine[0] and mine[0].endswith('ssid T'), 'scan finds T with the right BSSID, channel 6, open (%s)' % (mine[:1],))
j = n.cmd('join T', 20)[-1].split()
check(j[1] == '0' and int(j[3]) < 2000, 'join the open network in < 2 s: %s' % ' '.join(j))
ev = p.s.read(4096).decode(errors='replace')
check('event ap-join 02:4e:44:46:00:02' in ev, 'the peer reports the association')
for tries in range(1, 4):                                    # ARP is retried by the IP stack: the first request right after association can be lost
    a = n.cmd('arp 192.168.4.1', 10)[-1]
    if 'reply 1' in a: break
check('reply 1 from 30:30:f9:5a:c1:11' in a, 'ARP request answered through the AP in %d tries (%s)' % (tries, a))
# AP vanishes: link-down within 3 beacon intervals + 1 s (about 1.3 s) of the last beacon, and a later join works
p.cmd('wifi_stop'); t0 = time.time()
st = n.cmd('poll 3000', 10)[-1]
check('up 0' in st, 'AP gone: link reported down (%s)' % st)
p.cmd('wifi_ap T - 6'); time.sleep(1)
j = n.cmd('join T', 20)[-1].split()
check(j[1] == '0', 'join again after the AP came back: %s' % ' '.join(j))
# a deauth from some other BSSID must not drop us
forged = 'c0000000' + '024e44460002' + '020000000099' + '020000000099' + '0000' + '0300'
p.cmd('wifi_raw ap ' + forged); time.sleep(0.3)
check('up 1' in n.cmd('poll 500', 10)[-1], 'deauth from a different BSSID is ignored')
real = 'c0000000' + '024e44460002' + '3030f95ac111' + '3030f95ac111' + '0000' + '0300'
p.cmd('wifi_raw ap ' + real); time.sleep(0.3)
check('up 0' in n.cmd('poll 500', 10)[-1], 'deauth from the AP itself takes the link down')
print('FAIL: %s' % fail if fail else 'sta: all passed'); sys.exit(1 if fail else 0)
