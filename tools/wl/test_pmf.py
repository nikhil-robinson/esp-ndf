#!/usr/bin/env python3
"""PMF (802.11w) and WPA3-SAE on the station (components/lwip/test_apps/ip_unit) against the IDF AP on the peer: WPA2 with PMF capable and PMF required, WPA3 (SAE) and the WPA2/WPA3 transition mode; with SAE a wrong password must fail (-3).
The BOX-3 (BOX3 in make/boards.mk) forges frames: an unprotected deauthentication (unicast and broadcast) from the AP's BSSID must not drop the
link, an association request in our name makes the AP send an SA Query that we must answer (else the AP drops us), and the AP's own
(protected) deauthentication must take the link down. On an open network the same forged deauthentication drops the link (the control case: the IDF WPA2 AP is PMF capable by default)."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board, ap_bssid
from ndf import Ndf

PASS = 'ndfpass123'; ME = '024e44460002'
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)

p = Peer(); box = Peer(_board('BOX3', ''))
def link(n, ms=0): return n.cmd('link %d' % ms, ms / 1000 + 10)[-1] == 'link 1'
def deauth(bss, dst): return 'c0000000' + dst + bss + bss + '0000' + '0700'
def assoc(bss): return '00000000' + bss + ME + bss + '0000' + '3104' + '0a00' + '0001' + 'T'.encode().hex() + '010482848b96' + '30140100000fac040100000fac040100000fac028000'
for mode in ('pmf:', 'pmfreq:', 'wpa3:', 'wpa23:', ''):
    p.reboot(); box.reboot(); time.sleep(3)
    p.cmd('wifi_stop'); bss = ap_bssid(p.cmd('wifi_ap T %s 6' % (mode + PASS if mode else '-'))).replace(':', ''); time.sleep(1)
    box.cmd('wifi_stop'); box.cmd('wifi_ap junk - 6')       # a transmitter on channel 6
    n = Ndf(ready='ip_unit ready')
    r = n.cmd('up T' + (' ' + PASS if mode else ''), 40)[-1]
    name = mode[:-1] if mode else 'open'
    check(r.startswith('up rc 0'), '%s: joined (%s)' % (name, r))
    if not r.startswith('up rc 0'): continue
    if mode.startswith('wpa3'):
        w = n.cmd('up T wrongpass1', 40)[-1]
        check(w.startswith('up rc -3'), '%s: wrong password refused by SAE (-3) (%s)' % (name, w))
        r = n.cmd('up T ' + PASS, 40)[-1]
        check(r.startswith('up rc 0'), '%s: joined again with the right password (%s)' % (name, r))
    for _ in range(3): box.cmd('wifi_raw ap ' + deauth(bss, ME)); time.sleep(0.05)
    up = link(n, 500)
    if not mode:
        check(not up, 'open: a forged deauthentication drops the link (control case: the forged frames reach us)'); continue
    check(up, '%s: forged unprotected unicast deauthentication ignored' % name)
    for _ in range(3): box.cmd('wifi_raw ap ' + deauth(bss, 'ffffffffffff')); time.sleep(0.05)
    check(link(n, 500), '%s: forged broadcast deauthentication without a valid MMIE ignored' % name)
    for _ in range(3): box.cmd('wifi_raw ap ' + assoc(bss)); time.sleep(0.1)
    up = link(n, 4000); pr = n.cmd('ping 192.168.4.1 5', 30)[-1] if up else 'down'
    check(up and 'recv 0' not in pr, '%s: association request in our name answered by our SA Query response, link kept (%s)' % (name, pr))
    p.cmd('wifi_kick')
    check(not link(n, 1500), '%s: the AP\'s protected deauthentication takes the link down' % name)
print('FAIL: %s' % fail if fail else 'pmf: all passed'); sys.exit(1 if fail else 0)
