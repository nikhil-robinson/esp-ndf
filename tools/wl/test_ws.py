#!/usr/bin/env python3
"""WebSocket (components/net/ws.c) on http_unit: the esp-ndf station serves ws://.../ws (echo) next to HTTP on port 80; the IDF peer is the AP
and runs esp_http_server's WebSocket echo on :8080/ws; the BOX-3 joins as an IDF station and runs esp_websocket_client. Checked: IDF client
to our server (1000 and 4096-byte binary messages, text, a message above our limit is refused with close 1009, HTTP still answers on the same
port), our client to the IDF server (binary, text, ping, close handshake), and the same echoes IDF to IDF for timing."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf

fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)
def field(line, k): m = re.search(r'\b' + k + r' (\d+)', line); return int(m.group(1)) if m else -1

p = Peer(); box = Peer(_board('BOX3', '')); p.reboot(); box.reboot(); time.sleep(3)
import atexit; atexit.register(lambda: box.cmd('wifi_stop'))   # never leave the BOX-3 a station: after an AP restart it keeps its old address and collides with the next suite's DHCP lease
p.cmd('wifi_stop'); p.cmd('wifi_ap T - 6'); time.sleep(1); p.cmd('ws_serve')
n = Ndf(ready='http_unit ready', timeout=20)
r = n.cmd('up T', 40)[-1]; check(r.startswith('up rc 0'), 'joined (%s)' % r)
n.cmd('serve 80'); n.cmd('wsserve')
box.cmd('wifi_stop'); box.cmd('wifi_sta T -', 40)

g = box.cmd('ws_get ws://192.168.4.2/ws 20 1000', 120)[-1]
check(field(g, 'echoes') == 20 and field(g, 'text') == 1, 'IDF client -> our server: 20 binary echoes of 1000 bytes and a text one (%s)' % g)
g4 = box.cmd('ws_get ws://192.168.4.2/ws 5 4096', 120)[-1]
check(field(g4, 'echoes') == 5, 'IDF client -> our server: 4096-byte messages (NDF_WS_MSG_MAX) (%s)' % g4)
gb = box.cmd('ws_get ws://192.168.4.2/ws 1 4100', 120)[-1]
check(field(gb, 'echoes') == 0, 'a message above NDF_WS_MSG_MAX is refused (close 1009) (%s)' % gb)
h = [l for l in box.cmd('http_get http://192.168.4.2/hello 1', 60) if 'http' in l]
check(any('ok=1' in l for l in h), 'HTTP on the same port still answers after the WebSocket sessions (%s)' % h[-1:])

o = n.cmd('wsget ws://192.168.4.1:8080/ws 20 1000', 120)[-1]
check(field(o, 'rc') == 0 and field(o, 'echoes') == 20 and field(o, 'text') == 1 and field(o, 'ping') == 1 and field(o, 'closed') == 1,
      'our client -> IDF server: 20 binary echoes, text, ping, close handshake (%s)' % o)
o4 = n.cmd('wsget ws://192.168.4.1:8080/ws 5 4096', 120)[-1]
check(field(o4, 'echoes') == 5, 'our client -> IDF server: 4096-byte messages (%s)' % o4)
bad = n.cmd('wsget ws://192.168.4.1:8080/nows 1 10', 60)[-1]
check(field(bad, 'rc') == -1 if 'rc -1' in bad else False, 'a path without a WebSocket handler: the handshake fails (%s)' % bad)
ii = box.cmd('ws_get ws://192.168.4.1:8080/ws 20 1000', 120)[-1]
print('     ours -> IDF server: %s' % o)
print('     IDF  -> IDF server: %s' % ii)
print('     IDF  -> our server: %s' % g)
print('FAIL: %s' % fail if fail else 'ws: all passed'); sys.exit(1 if fail else 0)
