#!/usr/bin/env python3
"""HTTP test (components/net/test_apps/http_unit): the IDF peer is an open AP with an HTTP client and server; the esp-ndf station serves /hello and
a 64 KB /big, fetches the peer's chunked 64 KB /big, and survives malformed requests with its memory back at baseline."""
import os, sys, time, re
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf

p = Peer(); p.reboot()
n = Ndf(ready='http_unit ready', timeout=20)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)

p.cmd('wifi_stop'); p.cmd('wifi_ap T - 6 0'); time.sleep(1)
u = n.cmd('up T', 30)[-1]
m = re.search(r'ip (\S+)', u); ip = m.group(1) if m and m.group(1) != '-' else None
check(ip is not None, 'join + DHCP: %s' % u)
n.cmd('serve 80'); time.sleep(0.2)
try: p.cmd('http_get http://%s/hello 3' % ip, 60)
except RuntimeError: pass                                            # the first requests can lose to ARP
time.sleep(12)         # warm-up: one-time allocations (ARP entries, timers) happen before the baseline
base = int(n.cmd('memused')[-1].split()[1])
r = p.cmd('http_get http://%s/hello' % ip, 30)
check('http status=200 bytes=5' in r, 'peer GET /hello -> 200 "hello" (%s)' % r)
r = p.cmd('http_get http://%s/hello 100' % ip, 300)
check(any(l.startswith('http ok=100 of 100') for l in r), '100 requests answered (%s)' % r[-1])
r = p.cmd('http_get http://%s/big' % ip, 120)
check('http status=200 bytes=65536' in r, 'peer GET /big: 64 KB (%s)' % r)
for kind, want in (('nocrlf', 'HTTP/1.1 400'), ('badline', 'HTTP/1.1 400'), ('longline', 'HTTP/1.1')):
    r = p.cmd('http_raw %s 80 %s' % (ip, kind), 30)
    check(any(want in l or l.endswith('closed') for l in r), 'malformed request "%s": 400 or closed (%s)' % (kind, r))
time.sleep(1)
r = p.cmd('http_get http://%s/hello 5' % ip, 60)
check(any(l.startswith('http ok=5 of 5') for l in r), 'still serving after the malformed requests')
time.sleep(12)                                              # idle connections are closed after 10 s
mem = int(n.cmd('memused')[-1].split()[1])
check(mem <= base + 32, 'lwIP heap back at baseline: %d -> %d bytes' % (base, mem))
p.cmd('http_serve 80'); time.sleep(1.5)                        # the peer serves /hello and /big (chunked); give its server a moment
g = n.cmd('get http://192.168.4.1/hello', 30)[-1]
check('status 200 bytes 5 ' in g, 'we GET the peer /hello: %s' % g)
g = n.cmd('get http://192.168.4.1/big', 120)[-1]
check('status 200 bytes 65536 bad 0' in g, 'we GET the peer chunked 64 KB /big intact: %s' % g)
print('FAIL: %s' % fail if fail else 'http: all passed'); sys.exit(1 if fail else 0)
