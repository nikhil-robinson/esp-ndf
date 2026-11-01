#!/usr/bin/env python3
"""BLE link-layer procedures against the IDF central (peer image): connection parameter update, 2M PHY, notifications keep flowing without gaps through each change, and
the 2M burst. Needs examples/bluetooth/ble_gatts on the DEVKIT.  The peer reports its own view of the link (ble_info)."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf
ADDR = 'c6:55:44:33:22:11'
ndf = Ndf(ready='advertise rc 0', timeout=20)
p = Peer(); time.sleep(5)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)
def info():
    l = p.cmd('ble_info')[-1]; return {k: int(v) for k, v in re.findall(r'(\w+) (-?\d+)', l)}
def stats():
    s = p.cmd('ble_stats')[-1].split(); return int(s[1]), int(s[3])
p.cmd('ble_adv_stop', 5); p.cmd('ble_connect %s 1' % ADDR, 20); p.cmd('ble_disc', 30); p.cmd('ble_sub 10', 8); time.sleep(2)
i0 = info(); print(i0)
check(i0['txphy'] == 1 and i0['rxphy'] == 1, 'link starts on the 1M PHY')
n0, g0 = stats(); time.sleep(2); n1, g1 = stats()
check(n1 - n0 >= 15 and g1 == 0, 'notifications flow (%d in 2 s, gaps %d)' % (n1 - n0, g1))
# connection parameter update: the central sends LL_CONNECTION_UPDATE_IND
iv0 = i0['itvl']; new = 24 if iv0 != 24 else 40
p.cmd('ble_update %d %d 0 400' % (new, new), 8); time.sleep(3)
i1 = info(); print(i1)
check(i1['itvl'] == new, 'interval %d -> %d (peer view)' % (iv0, i1['itvl']))
n0, g0 = stats(); time.sleep(3); n1, g1 = stats()
check(n1 - n0 >= 20 and g1 == g0 == 0, 'notifications flow after the update (%d in 3 s, gaps %d)' % (n1 - n0, g1))
p.cmd('ble_update %d %d 0 400' % (iv0, iv0), 8); time.sleep(3)
check(info()['itvl'] == iv0, 'back to interval %d' % iv0)
# 2M PHY
p.cmd('ble_phy 2', 8); time.sleep(3)
i2 = info(); print(i2)
check(i2['txphy'] == 2 and i2['rxphy'] == 2, 'PHY update to 2M (peer view tx %d rx %d)' % (i2['txphy'], i2['rxphy']))
n0, g0 = stats(); time.sleep(3); n1, g1 = stats()
check(n1 - n0 >= 20 and g1 == g0, 'notifications flow on 2M (%d in 3 s, gaps %d)' % (n1 - n0, g1))
p.cmd('ble_write 7 b5', 8); time.sleep(6.5)
n, g = stats(); print('2M burst: %d notifications in 5 s = %d/s, gaps %d' % (n, n // 5, g))
check(g == 0 and n // 5 >= 700, '2M burst without gaps (%d/s)' % (n // 5))
p.cmd('ble_phy 1', 8); time.sleep(3)
i3 = info()
check(i3['txphy'] == 1 and i3['rxphy'] == 1, 'PHY back to 1M')
p.cmd('ble_disconnect', 5)
print('PASS ble_link' if not fail else 'FAIL ble_link: %s' % fail)
sys.exit(1 if fail else 0)
