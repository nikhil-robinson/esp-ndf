#!/usr/bin/env python3
"""BLE GATT soak with the esp-ndf board's console port open but never read (as soak_wl.py holds it): IDF central on the PEER connects,
discovers, takes 10 s of notifications, disconnects; repeated for <minutes> (default 20). Data: bench/idf_wl_peer/data/soak_ble_unread.txt"""
import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer
from ndf import Ndf
mins = float(sys.argv[1]) if len(sys.argv) > 1 else 20
p = Peer(); p.reboot(); p.cmd('wifi_stop')
n = Ndf(ready='advertise rc 0', timeout=20); ADDR = 'c6:55:44:33:22:11'; t_end = time.time() + mins * 60; r = fails = 0
while time.time() < t_end:
    r += 1
    try:
        p.cmd('ble_adv_stop', 5); p.cmd('ble_connect %s 1' % ADDR, 20); p.cmd('ble_disc', 30); p.cmd('ble_sub 10', 8); time.sleep(10)
        cnt = int(p.cmd('ble_stats')[-1].split()[1]); p.cmd('ble_disconnect', 5); time.sleep(2); fails += cnt < 80
    except Exception as e:
        fails += 1; print(time.strftime('%H:%M:%S'), 'round', r, e, flush=True)
        try: p.cmd('ble_disconnect', 5)
        except Exception: pass
        time.sleep(5)
print('rounds %d fails %d' % (r, fails)); sys.exit(1 if fails else 0)
