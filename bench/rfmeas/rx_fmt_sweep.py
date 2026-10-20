#!/usr/bin/env python3
"""RX format sweep on the passive-scan template: which CS format receives long (100 B) packets and never transmits
(D0 hears no reply)? Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time
for f in range(32):
    tx = rfhost.Board(rfhost.D0); tx.cmd('init')
    rx = rfhost.Board(rfhost.C948); rx.cmd('init')
    res = {}
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rawrx 37 0 0x71764129 2500 0:0 2 %d' % (0x20 + f), timeout=20))); th.start()
    time.sleep(0.8)
    try: a = tx.cmd('rawsend 37 0 0x71764129 100 2', timeout=10)[0]
    except Exception: a = 'tx?'
    th.join(timeout=25); out = res.get('rx', ['-', '-', '-'])
    print('fmt %2d: D0 %s | %s | %s' % (f, a.replace('rawsend rc 0 ', ''), out[-2].replace('rawrx err bits: ', '') if len(out) > 1 else '', out[-1].split(' core374')[0].replace('rawrx rc 0 ', '')), flush=True)
