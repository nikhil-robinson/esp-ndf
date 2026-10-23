#!/usr/bin/env python3
"""Static RX IQ imbalance: beta averaged over 4 captures at 36 dB gain (2440 MHz, tone +3 MHz baseband), applied to 4 fresh
captures at 36 and 32 dB, and at tone +5 MHz. Raw vs corrected image rejection. Run from tools/ with PYTHONPATH=."""
import rfhost, numpy as np
sx = lambda v: v - 1024 if v & 512 else v
dft = lambda x, f: np.sum(x * np.exp(-2j * np.pi * f * np.arange(len(x)) / 16e6))
rr = lambda z, f: 10 * np.log10(abs(dft(z, f)) ** 2 / abs(dft(z, -f)) ** 2)
def cap(khz, g):
    lines = rx.cmd('iqcap %d %d' % (khz, -g), timeout=60)
    w = [int(l[k:k + 5], 16) for l in lines[1:] for k in range(0, len(l), 5)]
    x = np.array([sx(v & 0x3FF) for v in w], float) - 1j * np.array([sx(v >> 10 & 0x3FF) for v in w], float)
    return x - x.mean()
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); tx.cmd('tone 2440000 1000', timeout=10)
cal = [cap(2438000, 36) for _ in range(4)]
beta = np.mean([dft(x, -3e6) / np.conj(dft(x, 3e6)) for x in cal]); print('beta |%.4f| %.0f deg' % (abs(beta), np.degrees(np.angle(beta))))
for g, khz, f in ((36, 2438000, 3e6), (32, 2438000, 3e6), (36, 2436000, 5e6)):
    r = [(rr(x, f), rr(x - beta * np.conj(x), f)) for x in (cap(khz, g) for _ in range(4))]
    print('gain %d tone +%d MHz: raw %s -> corrected %s' % (g, f / 1e6, ['%.1f' % a for a, b in r], ['%.1f' % b for a, b in r]), flush=True)
tx.cmd('stop')
