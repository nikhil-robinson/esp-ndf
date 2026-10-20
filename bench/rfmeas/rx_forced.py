#!/usr/bin/env python3
"""RX-only by the front end: C9:48 receives in format 0x03 with the PHY bus forced to RX (TX path off, gain code g);
D0 raw-sends 251 bytes and counts replies it hears. Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time
for g, dg in ((32, None), (64, None), (100, None), (127, None), (200, None), (300, None), (511, None), (0x189, None), (0x184, None)):
    tx = rfhost.Board(rfhost.D0); tx.cmd('init')
    rx = rfhost.Board(rfhost.C948); rx.cmd('init')
    res = {}
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rawrx 37 0 0x71764129 2500 0:0 2 3 0xE000 %d%s' % (g, (' ' + dg) if dg else ''), timeout=20))); th.start()
    time.sleep(0.8)
    a = tx.cmd('rawsend 37 0 0x71764129 251 2', timeout=10)[0]
    th.join(); out = res['rx']
    print('force %3d txgain(0,0,%s): D0 %s | %s | %s' % (g, dg, a.replace('rawsend rc 0 ', ''), out[-2].replace('rawrx err bits: ', ''), out[-1].split(' core374')[0].replace('rawrx rc 0 ', '')), flush=True)
