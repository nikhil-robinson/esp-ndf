#!/usr/bin/env python3
"""Wireless soak against the IDF peer: Wi-Fi station over WPA2 (ping, UDP, TCP rounds), then the access point with the IDF station, then the BLE GATT server with the IDF
central. Every round is timestamped into bench/soak/data/wireless.log; a round fails below its threshold, the lwIP heap (memused after the round) must stay flat.
usage: soak_wl.py [minutes-sta minutes-ap minutes-ble]   (default 25 15 20)"""
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf, DEVKIT
ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
mins = [float(a) for a in sys.argv[1:4]] + [25, 15, 20][len(sys.argv[1:4]):]
log = open(os.path.join(ROOT, 'bench', 'soak', 'data', 'wireless.log'), 'a')
rounds = fails = 0
def say(s, bad=False):
    global fails
    fails += bad
    line = '%s %s%s' % (time.strftime('%Y-%m-%d %H:%M:%S'), 'FAIL ' if bad else '', s); log.write(line + '\n'); log.flush(); print(line, flush=True)
def flash(app):
    r = subprocess.run(['make', '-C', os.path.join(ROOT, app), '-j4', 'flash', 'PORT=' + DEVKIT], capture_output=True, text=True)
    if r.returncode: say('build/flash of %s failed: %s' % (app, r.stdout[-300:] + r.stderr[-300:]), True); sys.exit(1)
def num(pat, text, default=-1):
    m = re.search(pat, text); return int(m.group(1)) if m else default

p = Peer(); say('soak start: sta %.0f min, ap %.0f min, ble %.0f min' % tuple(mins))

# ---- 1. station, WPA2 ----
flash('components/lwip/test_apps/ip_unit'); p.reboot()
p.cmd('wifi_stop'); p.cmd('wifi_ap T secret123 6 0'); time.sleep(1); p.cmd('udp_echo 7'); p.cmd('tcp_echo 8')
n = Ndf(ready='ip_unit ready', timeout=20)
u = n.cmd('up T secret123', 40)[-1]; ip = re.search(r'ip (\S+)', u).group(1) if 'ip -' not in u else None
if not ip: say('station did not get an address: ' + u, True)
else:
    base = None; t_end = time.time() + mins[0] * 60
    while time.time() < t_end:
        rounds += 1
        r = [l for l in p.cmd('ping %s 20' % ip, 60) if 'ping ok=' in l]; peer_ok = num(r'ok=(\d+)', r[-1] if r else '')
        ours = num(r'recv (\d+)', n.cmd('ping 192.168.4.1 20', 60)[-1])
        udp = num(r'ok (\d+)', n.cmd('udp 192.168.4.1 7 100', 300)[-1])
        tcp = n.cmd('tcp 192.168.4.1 8 2048', 200)[-1]
        mem = num(r'mem used (\d+)', n.cmd('stats')[-1])
        if base is None: base = mem
        bad = peer_ok < 16 or ours < 16 or udp < 60 or 'rcvd 2048 bad 0' not in tcp or mem > base + 64
        say('round %d sta: peer-ping %d/20 our-ping %d/20 udp %d/100 tcp %s heap %d' % (rounds, peer_ok, ours, udp, 'ok' if 'rcvd 2048 bad 0' in tcp else tcp, mem), bad)
n.close()

# ---- 2. access point ----
flash('components/wifi/test_apps/ap_unit'); p.reboot(); p.cmd('wifi_stop')
n = Ndf(ready='ap_unit ready', timeout=20); n.cmd('start ndf-ap secret123', 10); n.cmd('echo 8')
try: p.cmd('wifi_sta ndf-ap secret123', 40)
except RuntimeError as e: say('peer station could not join the access point: %s' % e, True)
base = None; t_end = time.time() + mins[1] * 60
while time.time() < t_end:
    rounds += 1
    r = [l for l in p.cmd('ping 192.168.4.1 20', 60) if 'ping ok=' in l]; ok = num(r'ok=(\d+)', r[-1] if r else '')
    try: tcp = p.cmd('tcp_send 192.168.4.1 8 1024', 120)[-1]
    except RuntimeError as e: tcp = str(e)[:60]
    st = n.cmd('apstat')[-1]; mem = num(r'mem used (\d+)', n.cmd('stats')[-1]); used = num(r'used (\d+)$', st)
    if base is None: base = mem
    bad = ok < 16 or 'echoed=1024 bad=0' not in tcp or used != 1 or mem > base + 64
    say('round %d ap: peer-ping %d/20 tcp %s stations %d heap %d' % (rounds, ok, 'ok' if 'echoed=1024 bad=0' in tcp else tcp, used, mem), bad)
n.close()

# ---- 3. BLE GATT server ----
flash('examples/bluetooth/ble_gatts'); p.reboot(); p.cmd('wifi_stop')
n = Ndf(ready='advertise rc 0', timeout=20); ADDR = 'c6:55:44:33:22:11'; t_end = time.time() + mins[2] * 60
while time.time() < t_end:
    rounds += 1
    try:
        p.cmd('ble_adv_stop', 5); p.cmd('ble_connect %s 1' % ADDR, 20); d = p.cmd('ble_disc', 30)
        p.cmd('ble_sub 10', 8); time.sleep(10); st = p.cmd('ble_stats')[-1].split(); cnt, gaps = int(st[1]), int(st[3])
        p.cmd('ble_disconnect', 5); time.sleep(2)
        say('round %d ble: discovered %d attributes, %d notifications in 10 s, gaps %d' % (rounds, len(d), cnt, gaps), cnt < 80 or gaps)
    except Exception as e:
        say('round %d ble: %s' % (rounds, e), True); time.sleep(5)
say('=== soak wireless: rounds %d fails %d ===' % (rounds, fails)); sys.exit(1 if fails else 0)
