#!/usr/bin/env python3
"""Fine sweep of TX analog registers 6B.02 (orig 0x3a) and 6B.04 (orig 0x08): tone and carrier vs value, each measured
against the original value taken just before (drift-proof). Run from tools/ with PYTHONPATH=."""
import rfhost
tx = rfhost.Board(rfhost.D0); tx.cmd('tone 2440000 1000 300', timeout=60); rx = rfhost.Board(rfhost.C948)
m = lambda: [float(rx.cmd('tonedb 2438000 %d 20' % f, timeout=30)[0].split()[1]) for f in (3000000, 2000000)]
for reg, orig, vals in (('02', 0x3a, range(0, 0x40, 2)), ('04', 0x08, range(0, 0x10))):
    for v in vals:
        tx.cmd('awr 6b %s %02x' % (reg, orig)); r = m()
        tx.cmd('awr 6b %s %02x' % (reg, v)); x = m()
        print('6b.%s = %02x: tone %+.1f  carrier %+.1f dB' % (reg, v, x[0] - r[0], x[1] - r[1]), flush=True)
    tx.cmd('awr 6b %s %02x' % (reg, orig))
