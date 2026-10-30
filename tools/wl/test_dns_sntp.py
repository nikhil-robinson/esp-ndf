#!/usr/bin/env python3
"""DNS and SNTP clients (components/lwip/net.c: net_dns, net_sntp) on the station (ip_unit) against servers on the IDF peer's AP (dns_serve,
sntp_serve), and the same lookups by an IDF station on the BOX-3 (getaddrinfo, esp_sntp) for comparison. Abuse: an unknown name (NXDOMAIN),
a DNS reply with the wrong transaction id (must be ignored: timeout), an SNTP reply that does not echo our timestamp (ignored) and a
kiss-of-death reply (stratum 0, refused)."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf

NAME, ADDR, T0 = 'ndf.test', '192.168.4.77', 1767225600       # 2026-01-01 00:00:00 UTC
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)

p = Peer(); box = Peer(_board('BOX3', '')); p.reboot(); box.reboot(); time.sleep(3)
import atexit; atexit.register(lambda: box.cmd('wifi_stop'))   # never leave the BOX-3 a station: after an AP restart it keeps its old address and collides with the next suite's DHCP lease
p.cmd('wifi_stop'); p.cmd('wifi_ap T - 6'); time.sleep(1)
p.cmd('dns_serve %s %s' % (NAME, ADDR)); p.cmd('sntp_serve %d' % T0); t_set = time.time()
n = Ndf(ready='ip_unit ready')
r = n.cmd('up T', 40)[-1]; check(r.startswith('up rc 0'), 'joined (%s)' % r)
def server_now(): return T0 + (time.time() - t_set)

d = [n.cmd('dns ' + NAME, 20)[-1] for _ in range(1)]
check('rc 0 ip ' + ADDR in d[0], 'net_dns resolves %s to %s (%s)' % (NAME, ADDR, d[0]))
c = n.cmd('dns ' + NAME, 20)[-1]; check('rc 0 ip ' + ADDR in c, 'second lookup from the cache (%s)' % c)
x = n.cmd('dns missing.test', 20)[-1]; check(x.startswith('dns rc -1') and int(x.split()[-1]) < 1000000, 'unknown name: NXDOMAIN, -1 at once (%s)' % x)
p.cmd('dns_serve other.test %s 1' % ADDR)
w = n.cmd('dns other.test', 30)[-1]; check(w.startswith('dns rc -') and ' ip - ' in w, 'reply with the wrong transaction id ignored (%s)' % w)
p.cmd('dns_serve %s %s' % (NAME, ADDR))

s = n.cmd('sntp 192.168.4.1', 20)[-1]; want = server_now()
m = re.search(r'rc 0 us (\d+) rtt (\d+) time (\d+)\.(\d+)', s)
got = int(m.group(3)) + int(m.group(4)) / 1e6 if m else 0
check(m and abs(got - want) < 0.5, 'net_sntp sets the clock to the server time within 0.5 s (%s; server about %.3f)' % (s, want))
p.cmd('sntp_serve %d 1' % int(server_now()))
s1 = n.cmd('sntp 192.168.4.1', 20)[-1]; check(s1.startswith('sntp rc -2'), 'SNTP reply without our timestamp ignored (%s)' % s1)
p.cmd('sntp_serve %d 2' % int(server_now()))
s2 = n.cmd('sntp 192.168.4.1', 20)[-1]; check(s2.startswith('sntp rc -3'), 'kiss of death (stratum 0) refused (%s)' % s2)
p.cmd('sntp_serve %d' % int(server_now()))
ours = [n.cmd('sntp 192.168.4.1', 20)[-1] for _ in range(3)]

box.cmd('wifi_stop'); box.cmd('wifi_sta T -', 40)
idf_dns = [box.cmd('dns_get ' + NAME, 20)[-1] for _ in range(1)]
idf_sntp = [box.cmd('sntp_get 192.168.4.1', 30)[-1] for _ in range(3)]
print('     ours: dns %s | sntp %s' % (d[0], ' | '.join(ours)))
print('     IDF:  %s | %s' % (idf_dns[0], ' | '.join(idf_sntp)))
print('FAIL: %s' % fail if fail else 'dns_sntp: all passed'); sys.exit(1 if fail else 0)
