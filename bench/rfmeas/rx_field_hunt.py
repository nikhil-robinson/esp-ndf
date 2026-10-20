#!/usr/bin/env python3
"""Which connection-template field lets the silent scan template (format 8) take long packets? One field set at a time,
D0 raw-sends 100 bytes; D0 counts replies. Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time
for off, val in ((0, 0), (0x04, 0x1100), (0x02, 0x0801), (0x1e, 0x0021), (0x1e, 0x00ff), (0x22, 0xffff), (0x26, 0x0c1f), (0x50, 0x00a6), (0x56, 0x0100), (0x04, 0x1300), (0x04, 0x1500)):
    tx = rfhost.Board(rfhost.D0); tx.cmd('init')
    rx = rfhost.Board(rfhost.C948); rx.cmd('init')
    res = {}
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rawrx 37 0 0x71764129 2500 %#x:%#x 2 %d' % (off, val, 0x28), timeout=20))); th.start()
    time.sleep(0.8)
    a = tx.cmd('rawsend 37 0 0x71764129 100 2', timeout=10)[0]
    th.join(); out = res['rx']
    print('scan tmpl fmt 8 + CS+%#04x=%#06x: D0 %s | %s | %s' % (off, val, a.replace('rawsend rc 0 ', ''), out[-2].replace('rawrx err bits: ', ''), out[-1].split(' core374')[0].replace('rawrx rc 0 ', '')), flush=True)
