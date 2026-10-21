#!/usr/bin/env python3
"""Block 0x6B (TX analog): which register scales the whole TX output (tone AND carrier)? D0 tone at 2440 MHz + 1 MHz
(after the TX DC cal in rf_tx_start), each 6B register stepped over a few values; C9:48 measures tone and carrier.
Run from tools/ with PYTHONPATH=."""
import rfhost
tx = rfhost.Board(rfhost.D0); print(tx.cmd('tone 2440000 1000 300', timeout=60))
rx = rfhost.Board(rfhost.C948)
m = lambda: [float(rx.cmd('tonedb 2438000 %d 20' % f, timeout=30)[0].split()[1]) for f in (3000000, 2000000)]
base = m(); print('base: tone %.1f carrier %.1f' % tuple(base))
for r in range(0x00, 0x10):
    orig = int(tx.cmd('ard 6b %02x' % r)[0].split()[1], 16)
    row = []
    for val in (0x00, orig >> 1, orig ^ 0x0f, (orig + 0x40) & 0xff, 0xff):
        tx.cmd('awr 6b %02x %02x' % (r, val)); t, c = m(); row.append('%02x:%+.1f/%+.1f' % (val, t - base[0], c - base[1]))
    tx.cmd('awr 6b %02x %02x' % (r, orig))
    print('6b.%02x (orig %02x): %s' % (r, orig, '  '.join(row)), flush=True)
