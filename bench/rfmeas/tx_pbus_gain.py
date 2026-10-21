#!/usr/bin/env python3
"""TX analog gain on the PHY bus: row 5 col 1 (PA, default 1) and row 4 col 1 (default 127), stepped during a tone;
tone and carrier against the default taken just before each point (drift-proof). Run from tools/ with PYTHONPATH=."""
import rfhost
tx = rfhost.Board(rfhost.D0); tx.cmd('tone 2440000 1000 300', timeout=60); rx = rfhost.Board(rfhost.C948)
m = lambda: [float(rx.cmd('tonedb 2438000 %d 20' % f, timeout=30)[0].split()[1]) for f in (3000000, 2000000)]
for row, dflt, vals in ((5, 1, (0, 2, 3, 4, 6, 8, 10, 12, 15, 31, 63)), (4, 127, (0, 16, 32, 48, 64, 80, 96, 112, 120, 255))):
    for v in vals:
        tx.cmd('pbw %d 1 %d' % (row, dflt)); r = m()
        tx.cmd('pbw %d 1 %d' % (row, v)); x = m()
        print('pbus row %d col 1 = %3d: tone %+.1f carrier %+.1f dB' % (row, v, x[0] - r[0], x[1] - r[1]), flush=True)
    tx.cmd('pbw %d 1 %d' % (row, dflt))
