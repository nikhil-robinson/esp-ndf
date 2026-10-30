#!/usr/bin/env python3
"""HTTPS test (components/net/test_apps/https_unit): the IDF peer is an open AP with esp_http_client / esp_https_server; the esp-ndf station serves /hello and /big over
TLS 1.3 (our engine, certificate from tools/wl/pki) and fetches the peer's pages. Wrong CA and wrong host name are refused in both directions."""
import os, sys, time, re
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf

p = Peer(); p.reboot()
n = Ndf(ready='https_unit ready', timeout=20)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)
def peer_cmd(c, t=120):                                    # the output stays readable when the peer answers err
    try: return p.cmd(c, t)
    except RuntimeError as e: return str(e).split('\n')[1:] + ['ERR']

p.cmd('wifi_stop'); p.cmd('wifi_ap T - 6'); time.sleep(1)
u = n.cmd('up T', 30)[-1]
m = re.search(r'ip (\S+)', u); ip = m.group(1) if m and m.group(1) != '-' else None
check(ip is not None, 'join + DHCP: %s' % u)
n.cmd('serve 443'); time.sleep(0.3)
for i in range(3):
    r = peer_cmd('https_get https://%s/hello - 1' % ip, 60)
    if 'ERR' not in r: break
check('https status=200 bytes=5' in r, 'IDF client GET /hello from the esp-ndf server: 200 "hello" (%s)' % r)
r = peer_cmd('https_get https://%s/hello - 8' % ip, 400)
check(any(l.startswith('https ok=8 of 8') for l in r), '8 requests, one handshake each (%s)' % r[-2:])
r = peer_cmd('https_get https://%s/big - 1' % ip, 300)
check('https status=200 bytes=65536' in r, 'IDF client GET /big: 64 KB through our TLS records (%s)' % r)
r = peer_cmd('https_get https://%s/hello other 1' % ip, 60)
check('ERR' in r, 'IDF client with an unrelated CA refuses our certificate (%s)' % r)
r = peer_cmd('https_get https://%s/hello - 1 evil.example.com' % ip, 60)
check('ERR' in r, 'IDF client refuses our certificate for another host name (%s)' % r)
r = peer_cmd('https_get https://%s/hello - 1' % ip, 60)
check('https status=200 bytes=5' in r, 'the server still answers after the refused handshakes (%s)' % r)
r = peer_cmd('https_get https://%s/hello - 1 ndf.local 12' % ip, 60)
v = n.cmd('tlsver', 10)[-1]
check('https status=200 bytes=5' in r and v == 'tlsver 0303', 'IDF client forced to TLS 1.2: our server answers over 1.2 (%s, %s)' % (r[-1:], v))
r = peer_cmd('https_get https://%s/big - 1 ndf.local 12' % ip, 300)
check('https status=200 bytes=65536' in r, 'IDF client TLS 1.2 GET /big: 64 KB (%s)' % r[-1:])

p.cmd('https_serve 443'); time.sleep(2)
g = n.cmd('get https://192.168.4.1/hello ndf.local', 60)[-1]
check('status 200 bytes 5 ' in g and ' tls 0304 ' in g, 'we GET the IDF server /hello over TLS 1.3: %s' % g)
g = n.cmd('get https://192.168.4.1/hello ndf.local 12', 60)[-1]
check('status 200 bytes 5 ' in g and ' tls 0303 ' in g, 'we GET the IDF server /hello over TLS 1.2 (esp_https_server / mbedtls): %s' % g)
g = n.cmd('get https://192.168.4.1/big ndf.local 12', 200)[-1]
check('status 200 bytes 65536 bad 0' in g and ' tls 0303 ' in g, 'TLS 1.2 chunked 64 KB /big from the IDF server: %s' % g)
g = n.cmd('get https://192.168.4.1/big ndf.local', 200)[-1]
check('status 200 bytes 65536 bad 0' in g, 'we GET the IDF server chunked 64 KB /big: %s' % g)
g = n.cmd('get https://192.168.4.1/hello ndf.local other', 60)[-1]
check('bytes -1' in g, 'we refuse the IDF certificate with an unrelated CA: %s' % g)
g = n.cmd('get https://192.168.4.1/hello evil.example.com', 60)[-1]
check('bytes -1' in g, 'we refuse the IDF certificate for another host name: %s' % g)
print('FAIL: %s' % fail if fail else 'https: all passed'); sys.exit(1 if fail else 0)
