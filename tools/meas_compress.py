#!/usr/bin/env python3
"""A.2 compression: received tone vs DAC amplitude at several RX gains (input steps of 3 dB)."""
from rfhost import *
import math
rx, tx = link()
amps = [4, 6, 8, 11, 16, 22, 32, 45, 64, 90, 128]
lines = ['rx_gain,' + ','.join('%d' % a for a in amps) + ',input_db_at_1db_compression,peak_at_max']
for g in (0, 10, 20, 30, 40, 50):
    rx.cmd('gain %d' % g); out = []; pk = 0
    for a in amps:
        tx.cmd('tx 127 0 2000 %d' % a); m = rx.meas(16000, 3e6); out.append(m['bins'][0]); pk = m['peak']
    inp = [20 * math.log10(a / 4) for a in amps]
    # linear fit on the 4 weakest points above noise, then first point 1 dB below the line
    ref = out[2] - inp[2]
    comp = next((inp[i] for i in range(3, len(amps)) if out[i] < inp[i] + ref - 1), None)
    lines.append('%d,' % g + ','.join('%.1f' % x for x in out) + ',%s,%d' % ('%.1f' % comp if comp is not None else 'none', pk)); print(lines[-1])
open('../bench/rfmeas/rx_compression.csv', 'w').write('\n'.join(lines) + '\n'); tx.cmd('txoff')
