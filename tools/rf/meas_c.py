#!/usr/bin/env python3
"""C characterisation, relative (no calibrated instruments): LO skirt, TX spurs (DAC SFDR + image + LO leak),
ADC noise-limited ENOB, PLL lock range on both boards. Writes bench/rfmeas/c_*.csv.
Setup as rfhost.link(): D0 sends a +2 MHz DAC tone on a 2437 MHz LO, C9:48 receives on 2436 MHz (tone at +3 MHz)."""
import math, os, rfhost

OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'bench', 'rfmeas')
N = 16383
RBW = 1.5 * 80e6 / N                               # Hann noise bandwidth


def bins(b, fs):
    return b.meas(N, *fs)['bins']


rx, tx = rfhost.link()
rx.cmd('hann 1'); rx.cmd('gain 10')
f0 = float(rx.cmd('scan %d 2.95e6 3.05e6 500' % N)[0].split()[2])    # tone incl. crystal offset
df = f0 - 3e6
tone = bins(rx, [f0])[0]

# LO skirt (TX LO + RX LO combined), and the floor with TX off at the same bins
offs = [-1e6, -500e3, -200e3, -100e3, -50e3, -20e3, 20e3, 50e3, 100e3, 200e3, 500e3, 1e6]
on = bins(rx, [f0 + o for o in offs])
# TX spurs relative to the tone (RX bin = TX IF + 1 MHz): LO leak, image, 2nd/3rd harmonics of the DAC tone
spur = {'lo_leak': 1e6, 'image': -1e6, 'h2(+4M)': 5e6, 'h3(-6M)': -5e6, 'h3(+6M)': 7e6, 'h2(-4M)': -3e6}
sp = bins(rx, [v + df for v in spur.values()])
tx.cmd('txoff')
off = bins(rx, [f0 + o for o in offs])
with open(os.path.join(OUT, 'c_lo_skirt.csv'), 'w') as f:
    f.write('# tone %.1f dB at %.0f Hz (gain 10, Hann, N %d, RBW %.0f Hz); dBc/Hz = dBc - 10log10(RBW)\n' % (tone, f0, N, RBW))
    f.write('offset_hz,dbc,dbc_hz,floor_dbc\n')
    for o, a, b in zip(offs, on, off):
        f.write('%.0f,%.1f,%.1f,%.1f\n' % (o, a - tone, a - tone - 10 * math.log10(RBW), b - tone))
        print('skirt %+8.0f Hz: %6.1f dBc (%6.1f dBc/Hz), floor %6.1f' % (o, a - tone, a - tone - 10 * math.log10(RBW), b - tone))
with open(os.path.join(OUT, 'c_tx_spurs.csv'), 'w') as f:
    f.write('spur,dbc\n')
    for k, v in zip(spur, sp):
        f.write('%s,%.1f\n' % (k, v - tone)); print('spur %-9s %6.1f dBc' % (k, v - tone))

# ADC: noise-limited ENOB at each gain, TX off (complex full scale = 512)
with open(os.path.join(OUT, 'c_adc_noise.csv'), 'w') as f:
    f.write('gain,rms,snr_fs_db,enob\n')
    for g in (0, 10, 20, 30, 40):
        rx.cmd('gain %d' % g); r = rx.meas(N)['rms']
        snr = 20 * math.log10(512 / r); f.write('%d,%.2f,%.1f,%.2f\n' % (g, r, snr, (snr - 1.76) / 6.02))
        print('adc gain %2d rms %.2f snr %.1f dB enob %.2f' % (g, r, snr, (snr - 1.76) / 6.02))

# PLL lock range, both boards
with open(os.path.join(OUT, 'c_lock_range.csv'), 'w') as f:
    f.write('board,khz,rc,cap,locked\n')
    for name, b in (('c948', rx), ('d0', tx)):
        for khz in range(1700000, 3000001, 25000):
            l = b.cmd('tunetx %d' % khz)[0].split()
            f.write('%s,%d,%s,%s,%s\n' % (name, khz, l[2], l[4], l[6]))
        b.cmd('tunetx 2437000')
print('done')
