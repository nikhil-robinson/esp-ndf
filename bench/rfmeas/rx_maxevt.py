#!/usr/bin/env python3
"""Format 0x03 RX (long packets work) with a small max event time: does the core then skip its reply?
D0 raw-sends 251 bytes and counts replies. Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time
for off, val in ((0, 0), (0x2e, 1), (0x2e, 2), (0x2e, 4), (0x2c, 1), (0x2c, 4), (0x2a, 1), (0x1e, 1), (0x20, 1)):
    tx = rfhost.Board(rfhost.D0); tx.cmd('init')
    rx = rfhost.Board(rfhost.C948); rx.cmd('init')
    res = {}
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rawrx 37 0 0x71764129 2500 %#x:%#x 2 3' % (off, val), timeout=20))); th.start()
    time.sleep(0.8)
    a = tx.cmd('rawsend 37 0 0x71764129 251 2', timeout=10)[0]
    th.join(); out = res['rx']
    print('fmt 3 + CS+%#04x=%#06x: D0 %s | %s | %s' % (off, val, a.replace('rawsend rc 0 ', ''), out[-2].replace('rawrx err bits: ', ''), out[-1].split(' core374')[0].replace('rawrx rc 0 ', '')), flush=True)
