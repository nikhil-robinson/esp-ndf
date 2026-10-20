#!/usr/bin/env python3
"""A.1 hop settling and A.3 TX power per gain step / bb code / frequency, two boards
(D0 transmits a +2 MHz DAC tone, C9:48 receives 1 MHz below). Writes bench/rfmeas/*.csv."""
from rfhost import *
out = '../bench/rfmeas/'
rx = Board(C948); tx = Board(D0)
rx.cmd('init'); tx.cmd('init'); rx.cmd('rxcal')
def cap(b, khz): return int(b.cmd('tunetx %d' % khz)[0].split()[4])
# A.1
tx.cmd('tunetx 2437000'); tx.cmd('tx 127 0 2000 300')
a, d = cap(rx, 2500000), cap(rx, 2436000); rx.cmd('gain 20')
with open(out + 'hop_settling.txt', 'w') as f:
    for rep in range(3):
        l = rx.cmd('hopmeas 2500000 %d 2436000 %d 3e6' % (a, d))[0]; f.write(l + '\n')
    f.write('steady %s\n' % rx.meas(8192, 3e6))
# A.3 gain steps
with open(out + 'tx_power_gain.csv', 'w') as f:
    f.write('tx_gain,bb,tone_db,image_db,leak_db\n')
    for bb in (0, 32, 128, 160, 256):
        for g in range(0, 128, 4 if bb == 0 else 16):
            tx.cmd('tx %d %d 2000 300' % (g, bb))
            m = rx.meas(8192, 3e6, -1e6, 1e6)
            f.write('%d,%d,%.1f,%.1f,%.1f\n' % (g, bb, *m['bins']))
        print('bb', bb, 'done')
# A.3 frequency
with open(out + 'tx_power_freq.csv', 'w') as f:
    f.write('mhz,tone_db,image_db,leak_db\n')
    for mhz in range(2240, 2761, 20):
        tx.cmd('tunetx %d' % (mhz * 1000)); tx.cmd('tx 127 0 2000 300')
        rx.cmd('tune %d' % ((mhz - 1) * 1000)); rx.cmd('gain 20')
        m = rx.meas(8192, 3e6, -1e6, 1e6)
        f.write('%d,%.1f,%.1f,%.1f\n' % (mhz, *m['bins']))
tx.cmd('txoff')
print('done')
