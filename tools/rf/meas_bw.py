#!/usr/bin/env python3
"""A.2 RX bandwidth code 67.04/67.05: response vs offset for each code (relative to 1 MHz, same pass)."""
from rfhost import *
rx, tx = link(); rx.cmd('tune 2437000'); rx.cmd('gain 10')
offs = [1000] + list(range(2000, 20001, 1000))
lines = ['code,' + ','.join(str(o) for o in offs) + ',corner_3db_khz']
for code in list(range(0, 0x40, 4)) + [0x3F]:
    rx.cmd('awr 0x67 4 0x%x' % code); rx.cmd('awr 0x67 5 0x%x' % code)
    r = []
    for f in offs:
        tx.cmd('tx 127 0 %d 128' % f); r.append(rx.meas(16000, f * 1000.0)['bins'][0])
    rel = [x - r[0] for x in r]
    corner = next((offs[i] for i in range(1, len(offs)) if rel[i] < -3), offs[-1])
    lines.append('0x%02x,' % code + ','.join('%.1f' % x for x in rel) + ',%d' % corner); print(lines[-1])
rx.cmd('awr 0x67 4 0x1f'); rx.cmd('awr 0x67 5 0x1f')
open('../bench/rfmeas/rx_bandwidth.csv', 'w').write('\n'.join(lines) + '\n'); tx.cmd('txoff')
