#!/usr/bin/env python3
"""A.7: crystal offset D0 vs C9:48 against D0 temperature while D0 transmits (self-heating), 3 minutes."""
from rfhost import *
import time
rx, tx = link(); rx.cmd('gain 10')
t0 = time.time()
with open('../bench/rfmeas/xtal_vs_temp.csv', 'w') as f:
    f.write('t_s,d0_temp_c,c948_temp_c,offset_hz,ppm\n')
    while time.time() - t0 < 180:
        tt = float(tx.cmd('temp')[0].split()[1]); tr = float(rx.cmd('temp')[0].split()[1])
        fr = sum(float(rx.cmd('freq 3e6')[0].split()[1]) for k in range(3)) / 3 - 3e6
        f.write('%.0f,%.1f,%.1f,%.0f,%.3f\n' % (time.time() - t0, tt, tr, fr, fr / 2437e6 * 1e6))
        time.sleep(5)
tx.cmd('txoff'); print(open('../bench/rfmeas/xtal_vs_temp.csv').read())
