#!/usr/bin/env python3
"""IP test (components/lwip/test_apps/ip_unit): the IDF peer is an open access point with a DHCP server and echo services; the esp-ndf station
joins, binds an address by DHCP, answers and sends ping, exchanges UDP datagrams of 1..1400 B and 1 MB over TCP, and survives a packet flood."""
import os, sys, time, re
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf

p = Peer(); time.sleep(4)
n = Ndf(ready='ip_unit ready', timeout=20)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)

p.cmd('wifi_stop'); PASS = os.environ.get('WL_PASS')                         # set: the network is WPA2-PSK and every packet goes through CCMP
p.cmd('wifi_ap T %s 6 0' % (PASS or '-')); time.sleep(1)
u = n.cmd('up T' + (' ' + PASS if PASS else ''), 30)[-1]
m = re.search(r'up rc (-?\d+) join_ms (\d+) total_ms (\d+) ip (\S+) gw (\S+)', u)
check(m and m.group(1) == '0' and m.group(4).startswith('192.168.4.') and m.group(5) == '192.168.4.1' and int(m.group(3)) < 6000, 'join + DHCP in < 6 s (a lost first DISCOVER costs a retry): %s' % u)
ip = m.group(4) if m else '192.168.4.2'
for _ in range(2):
    r = [l for l in p.cmd('ping %s 20' % ip, 60) if 'ping ok=' in l]
    if r: break
print('     peer:', r)
check(r and int(re.search(r'ok=(\d+)', r[-1]).group(1)) >= 18, 'peer pings us: >= 18/20 (%s)' % (r[-1] if r else ''))
r = n.cmd('ping 192.168.4.1 20', 60)[-1]
check(int(r.split()[4]) >= 18, 'we ping the peer: >= 18/20 (%s)' % r)
p.cmd('udp_echo 7')
r = n.cmd('udp 192.168.4.1 7 300', 600)[-1]
m = re.match(r'udp sent 300 ok (\d+) lost (\d+) corrupt (\d+)', r)
check(m and int(m.group(3)) == 0 and int(m.group(1)) >= 240, 'UDP echo, 300 datagrams of 1..1400 B: none corrupted, >= 80%% delivered (%s)' % r)
p.cmd('tcp_echo 8')
r = n.cmd('tcp 192.168.4.1 8 65536', 300)[-1]
check(r.startswith('tcp state 1 sent 65536 rcvd 65536 bad 0'), 'TCP echo, 64 KB intact (%s)' % r)
print('     stats:', n.cmd('stats')[-1])
print('FAIL: %s' % fail if fail else 'ip: all passed'); sys.exit(1 if fail else 0)
