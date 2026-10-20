#!/usr/bin/env python3
"""RX-only search: C9:48 receives in format 0x03 with CS+0x18 = val while D0 raw-sends 251 bytes; D0's events/s (390/s when
nobody answers, much higher when acknowledged) shows whether C9:48 transmitted. Run from tools/ with PYTHONPATH=."""
import rfhost, threading
for val in ('none', '0x0000', '0x2000', '0x4000', '0x8000', '0xE000', '0x1000', '0x0800', '0x0400', '0x0100'):
    tx = rfhost.Board(rfhost.D0); tx.cmd('init')
    rx = rfhost.Board(rfhost.C948); rx.cmd('init')
    if val == 'none':
        print('no receiver: D0', tx.cmd('rawsend 37 0 0x71764129 251 2', timeout=10)[0]); continue
    res = {}
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rawrx 37 0 0x71764129 2500 0:0 2 3 %s' % val, timeout=20))); th.start()
    import time; time.sleep(0.8)
    a = tx.cmd('rawsend 37 0 0x71764129 251 2', timeout=10)[0]
    th.join(); out = res['rx']
    print('CS+0x18 %s: D0 %s | %s | %s' % (val, a.replace('rawsend rc 0 ', ''), out[-2].replace('rawrx err bits: ', ''), out[-1].split(' core374')[0].replace('rawrx rc 0 ', '')), flush=True)
