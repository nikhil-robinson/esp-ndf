#!/usr/bin/env python3
"""A.2: receive gain per gain index (tone from D0, constant), noise floor (TX off)."""
from rfhost import *
rx = Board(C948); tx = Board(D0)
rx.cmd('init'); tx.cmd('init'); rx.cmd('rxcal')
tx.cmd('tunetx 2437000'); rx.cmd('tune 2436000')
rows = []
tx.cmd('tx 127 0 2000 30')                     # weak DAC tone (-20 dB) to stay linear at high gain
for g in range(128):
    rx.cmd('gain %d' % g); m = rx.meas(8192, 3e6); rows.append([g, m['bins'][0], m['rms'], m['peak']])
tx.cmd('txoff')
for g in range(128):
    rx.cmd('gain %d' % g); m = rx.meas(8192, 3e6); rows[g] += [m['bins'][0], m['rms']]
for g in range(128):
    rows[g].append(rx.cmd('rowget %d' % g)[0].split()[2:4])
with open('../bench/rfmeas/rx_gain.csv', 'w') as f:
    f.write('gain,tone_db,rms,peak,off_tone_db,off_rms,row_lo,row_hi\n')
    for r in rows: f.write('%d,%.1f,%.1f,%d,%.1f,%.1f,%s,%s\n' % (r[0], r[1], r[2], r[3], r[4], r[5], r[6][0], r[6][1]))
print('done')
