#!/usr/bin/env python3
"""A.2 RX baseband filter: response vs offset (D0 DAC tone at +f, both at 2437 MHz), baseline and with
block-0x67 candidate registers changed on the receiver."""
from rfhost import *
rx, tx = link(); rx.cmd('tune 2437000'); rx.cmd('gain 10')
offs = [500, 1000, 2000, 4000, 6000, 8000, 10000, 12000, 14000, 16000, 18000, 19000]
def resp():
    r = []
    for f in offs:
        tx.cmd('tx 127 0 %d 128' % f); r.append(rx.meas(16000, f * 1000.0)['bins'][0])
    return r
base = resp()
lines = ['offset_khz,' + ','.join(str(o) for o in offs), 'baseline,' + ','.join('%.1f' % x for x in base)]
print(lines[-1])
cands = [0x04, 0x05, 0x06, 0x07, 0x0C, 0x0D, 0x0E, 0x0F, 0x14, 0x15, 0x16, 0x17, 0x1C, 0x1D, 0x1E, 0x1F, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x30, 0x31, 0x34, 0x35]
for reg in cands:
    v = int(rx.cmd('ard 0x67 0x%x' % reg)[0].split()[1], 16)
    for nv in ((v // 2), min(0xFF, v * 2 + 1)):
        rx.cmd('awr 0x67 0x%x 0x%x' % (reg, nv)); r = resp(); rx.cmd('awr 0x67 0x%x 0x%x' % (reg, v))
        d = [a - b for a, b in zip(r, base)]
        lines.append('67.%02x %02x->%02x,' % (reg, v, nv) + ','.join('%.1f' % x for x in d))
        if max(abs(x) for x in d[4:]) > 3: print(lines[-1])
open('../bench/rfmeas/rx_filter.csv', 'w').write('\n'.join(lines) + '\n')
tx.cmd('txoff'); print('done')
