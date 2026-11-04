#!/usr/bin/env python3
"""A.3 TX output power controls (relative dB at the receiver): DAC amplitude, digital gain register."""
from rfhost import *
rx, tx = link(); rx.cmd('gain 4')
with open('../bench/rfmeas/tx_power_dac.csv', 'w') as f:
    f.write('dac_amplitude,tone_db,image_db\n')
    for a in (511, 362, 256, 181, 128, 90, 64, 45, 32, 22, 16, 11, 8, 6, 4, 3, 2):
        tx.cmd('tx 127 0 2000 %d' % a); m = rx.meas(16000, 3e6, -1e6); f.write('%d,%.1f,%.1f\n' % (a, *m['bins']))
with open('../bench/rfmeas/tx_power_dig.csv', 'w') as f:
    f.write('dig_gain_byte,tone_db\n')
    tx.cmd('tx 127 0 2000 300')
    for d in list(range(0, 0x80, 0x10)) + [0x7F] + list(range(0x80, 0x100, 0x10)) + [0xFF]:
        for a in range(0x60006014, 0x60006034, 4): tx.cmd('wr 0x%x 0x%x' % (a, d * 0x01010101))
        f.write('0x%02x,%.1f\n' % (d, rx.meas(16000, 3e6)['bins'][0]))
tx.cmd('txoff'); print('done')
