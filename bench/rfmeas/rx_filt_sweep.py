#!/usr/bin/env python3
"""Narrow RX filter search: response (dB, relative to 0.5 MHz) at offsets 0.5..8 MHz for analog block 0x67 settings.
D0 tone at 2440 MHz + f, C9:48 tonedb at 2440 MHz, fixed 20 dB gain. Settings: list of 'reg=val' (both written to
C9:48 by awr 0x67). Run from tools/ with PYTHONPATH=."""
import rfhost, sys
offs = [500, 1000, 1500, 2000, 3000, 4000, 6000, 8000]
cases = sys.argv[1:] or ['', '4=0x3f,5=0x3f', '4=0x40,5=0x40', '4=0x60,5=0x60', '4=0x80,5=0x80', '4=0xc0,5=0xc0', '4=0xff,5=0xff',
                         'c=0x3f,d=0x3f', '4=0x3f,5=0x3f,c=0x3f,d=0x3f', '4=0xff,5=0xff,c=0xff,d=0xff']
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948)
print('setting,' + ','.join(str(o) for o in offs), flush=True)
for case in cases:
    r = []
    for f in offs:
        tx.cmd('tone 2440000 %d' % f, timeout=10)
        rx.cmd('tonedb 2440000 %d 20' % (f * 1000), timeout=20)          # rx_start first, then the filter setting
        for kv in [c for c in case.split(',') if c]:
            k, v = kv.split('='); rx.cmd('awr 0x67 %#x %s' % (int(k, 16), v))
        r.append(float(rx.cmd('tonedb 2440000 %d 20 keep' % (f * 1000), timeout=20)[-1].split()[1]))
    print('%s,' % (case or 'default') + ','.join('%.1f' % (x - r[0]) for x in r), flush=True)
    rx.cmd('awr 0x67 4 0x1f'); rx.cmd('awr 0x67 5 0x1f'); rx.cmd('awr 0x67 0xc 0x1f'); rx.cmd('awr 0x67 0xd 0x1f')
tx.cmd('stop')
