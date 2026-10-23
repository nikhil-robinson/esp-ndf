#!/usr/bin/env python3
"""Raw RX image rejection vs held RX gain (dB), 3 captures each, tone at +3 MHz baseband (2440 MHz, D0 tone at +1 MHz,
RX LO -2 MHz). Also the single-tone beta per capture (|beta|, angle) to see if the imbalance is gain-dependent.
Run from tools/ with PYTHONPATH=."""
import rfhost, numpy as np, sys
sx = lambda v: v - 1024 if v & 512 else v
dft = lambda x, f: np.sum(x * np.exp(-2j * np.pi * f * np.arange(len(x)) / 16e6))
def cap(khz, g):
    lines = rx.cmd('iqcap %d %d' % (khz, -g), timeout=60)
    w = [int(l[k:k + 5], 16) for l in lines[1:] for k in range(0, len(l), 5)]
    x = np.array([sx(v & 0x3FF) for v in w], float) - 1j * np.array([sx(v >> 10 & 0x3FF) for v in w], float)
    return x - x.mean()
mhz = int(sys.argv[1]) if len(sys.argv) > 1 else 2440
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); tx.cmd('tone %d 1000' % (mhz * 1000), timeout=10)
for g in (16, 20, 24, 28, 32, 36):
    out = []
    for _ in range(3):
        x = cap((mhz - 2) * 1000, g); a, b = dft(x, 3e6), dft(x, -3e6); beta = b / np.conj(a)
        out.append('%.1f dB (|b| %.4f %4.0f deg, pk %d)' % (10 * np.log10(abs(a) ** 2 / abs(b) ** 2), abs(beta), np.degrees(np.angle(beta)), max(abs(x.real).max(), abs(x.imag).max())))
    print('gain %2d: %s' % (g, ' | '.join(out)), flush=True)
tx.cmd('stop')
