#!/usr/bin/env python3
"""Hidden demodulator modes? Every combination of modem 0x6C bits 27:24 and 0x70 bit 4 (the bits that select 1M / 2M /
coded) forced after rx start, against GFSK at rate:dev:len frames from D0. Prints combinations that receive.
Run from tools/ with PYTHONPATH=."""
import rfhost
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948)
for rate, dev, ln, gap in ((500, 125, 8, 100), (500, 250, 8, 100), (250, 62, 2, 20), (250, 125, 2, 20), (1000, 250, 20, 100), (2000, 500, 20, 100)):
    print('gfsk %d kbit/s dev %d len %d:' % (rate, dev, ln), tx.cmd('gfsk 2440 %d %d %d 200 %d' % (rate, dev, ln, gap), timeout=30)[0], flush=True)
    for eng in (0, 1):
        for m in range(16):
            for b4 in (0, 1):
                rx.cmd('rxset 0 0 0'); rx.cmd('rxset 0x6001106c 0x0f000000 %#x' % (m << 24)); rx.cmd('rxset 0x60011070 0x10 %#x' % (b4 << 4))
                l = [x for x in rx.cmd('rx 2440 %d 0x71764129 0 -1 150' % eng, timeout=8) if x.startswith('rx rc')][0].split()
                if int(l[4]) or int(l[6]): print('   engine rate %d, 0x6C[27:24]=%x 0x70b4=%d: good %s crcbad %s' % (eng, m, b4, l[4], l[6]), flush=True)
    rx.cmd('rxset 0 0 0')
tx.cmd('stop')
