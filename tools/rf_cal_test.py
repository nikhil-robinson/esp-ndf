#!/usr/bin/env python3
"""Calibration store test: D0 runs examples/rf/rf_cal, C9:48 runs examples/rf/rf_pkt (tone measurement).
1) erase, open (calibrates + stores), tone (TX DC cal + store): carrier/tone and times
2) reboot, open (loads), tone (no TX cal): times must drop, carrier must match
3) RX DC residual after a fresh cal and after a load.  PYTHONPATH=. python3 rf_cal_test.py"""
import rfhost
fails = 0
def check(n, c, info=''):
    global fails; print('%s %s  %s' % ('PASS' if c else 'FAIL', n, info), flush=True); fails += 0 if c else 1
rx = rfhost.Board(rfhost.C948)
def carrier():   # D0 tone at 2440 MHz + 1 MHz; receiver 2 MHz below: carrier at +2, tone at +3
    t = float(rx.cmd('tonedb 2438000 3000000 20', timeout=30)[0].split()[1]); c = float(rx.cmd('tonedb 2438000 2000000 20', timeout=30)[0].split()[1])
    return t, c - t
d = rfhost.Board(rfhost.D0); d.cmd('erase')
o1 = d.cmd('open', timeout=30)[0]; dc1 = d.cmd('dcres', timeout=30)[0]
t1 = d.cmd('tone 2440000 1000', timeout=30)[0]; tone1, c1 = carrier()
print('cold :', o1, '|', t1, '| tone %.1f carrier %.1f dBc' % (tone1, c1)); print('       ', dc1)
check('cold open calibrates and stores (source 3)', ' source 3 ' in o1)
d = rfhost.Board(rfhost.D0)
o2 = d.cmd('open', timeout=30)[0]; dc2 = d.cmd('dcres', timeout=30)[0]
t2 = d.cmd('tone 2440000 1000', timeout=30)[0]; tone2, c2 = carrier()
print('warm :', o2, '|', t2, '| tone %.1f carrier %.1f dBc' % (tone2, c2)); print('       ', dc2)
ms = lambda s, k: int(s.split(k)[1].split()[0])
check('warm open loads (source 1)', ' source 1 ' in o2)
check('warm open faster than cold', ms(o2, ' in ') < ms(o1, ' in '), '%d vs %d ms' % (ms(o2, ' in '), ms(o1, ' in ')))
check('warm tx_start skips the TX DC cal', ms(t2, 'tx_start ') + 20 < ms(t1, 'tx_start '), '%d vs %d ms' % (ms(t2, 'tx_start '), ms(t1, 'tx_start ')))
check('carrier after load within 3 dB of after cal', abs(c2 - c1) < 3, '%.1f vs %.1f dBc' % (c2, c1))
r1 = [abs(float(x)) for p in dc1.split()[1:] for x in p.split(':')[1].split('/')]; r2 = [abs(float(x)) for p in dc2.split()[1:] for x in p.split(':')[1].split('/')]
check('RX DC residual after load as good as after cal (max within +3 LSB)', max(r2) <= max(r1) + 3, 'max %.1f vs %.1f' % (max(r2), max(r1)))
print('=== %d failures' % fails)
