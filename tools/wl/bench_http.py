#!/usr/bin/env python3
"""HTTP server comparison on the same air (docs/PERFORMANCE.md): requests per second and a 64 KB transfer, esp-ndf http server (DEVKIT, http_unit) versus
ESP-IDF esp_http_server (BOX-3, bench/idf_wl_peer http_serve). The client is the IDF peer (esp_http_client, one connection per request) on its own AP "T"."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
peer = Peer(); box = Peer(_board('BOX3', '')); time.sleep(4)
import atexit; atexit.register(lambda: box.cmd('wifi_stop'))   # never leave the BOX-3 a station: after an AP restart it keeps its old address and collides with the next suite's DHCP lease
def cmd_out(c):
    try: return peer.cmd(c, 300)
    except RuntimeError as e: return str(e).split('\n')[1:]
def stats(ip, label, reps=60):
    out = {}
    for tries in range(4):                                                     # warm-up: ARP, first connections
        try: peer.cmd('http_get http://%s/hello 3' % ip, 60); break
        except RuntimeError: time.sleep(2)
    r = [l for l in cmd_out('http_get http://%s/hello %d' % (ip, reps)) if l.startswith('http ok=')][-1]
    m = re.search(r'ok=(\d+) of (\d+) elapsed_ms=(\d+)', r); ok, n, ms = int(m.group(1)), int(m.group(2)), int(m.group(3))
    big = [l for l in cmd_out('http_get http://%s/big 3' % ip) if l.startswith('http ok=')][-1]
    m = re.search(r'elapsed_ms=(\d+) bytes=(\d+)', big)
    print('%-12s /hello %d/%d in %d ms = %.1f req/s | /big x3: %d bytes in %d ms = %.1f KB/s' % (label, ok, n, ms, ok * 1000.0 / ms, int(m.group(2)), int(m.group(1)), int(m.group(2)) / 1.024 / int(m.group(1))))
peer.cmd('wifi_stop'); box.cmd('wifi_stop'); peer.cmd('wifi_ap T - 6 0'); time.sleep(1)
n = Ndf(ready='http_unit ready', timeout=20)
u = n.cmd('up T', 30)[-1]; ip = re.search(r'ip (\S+)', u).group(1); n.cmd('serve 80'); time.sleep(0.5)
stats(ip, 'esp-ndf')
n.close()
box.cmd('wifi_sta T -', 40); box.cmd('http_serve 80'); time.sleep(1.5)
stats('192.168.4.2', 'ESP-IDF')
