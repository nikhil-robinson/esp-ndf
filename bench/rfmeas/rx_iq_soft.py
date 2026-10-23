#!/usr/bin/env python3
"""Software RX IQ correction prototype: capture (iqcap, 16 MS/s) with D0's tone at +3 MHz baseband (TX image at +1),
autogain capture; single-tone estimate beta = X(-f) / conj(X(f)), correction y = x - beta conj(x)
(exact for a tone; also blind moments for comparison); image at -3 MHz before / after. Saves the captures in data/.
Run from tools/ with PYTHONPATH=."""
import rfhost, numpy as np, os
d = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')
def sx(v, b=10): return v - (1 << b) if v & (1 << (b - 1)) else v
def dft(x, f, fs=16e6): n = np.arange(len(x)); return np.sum(x * np.exp(-2j * np.pi * f * n / fs))
def tone(x, f): return abs(dft(x, f)) ** 2
def est(i, q):
    ii, qq, iq = np.mean(i * i), np.mean(q * q), np.mean(i * q)
    return np.sqrt(qq / ii), np.arcsin(iq / np.sqrt(ii * qq))
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948)
for mhz in (2404, 2440, 2478):
    tx.cmd('tone %d 1000' % (mhz * 1000), timeout=10)
    lines = rx.cmd('iqcap %d 200' % ((mhz - 2) * 1000), timeout=60)[1:]
    open(os.path.join(d, 'rx_iq_cap_%d.txt' % mhz), 'w').write('\n'.join(lines) + '\n')
    w = [int(l[k:k + 5], 16) for l in lines for k in range(0, len(l), 5)]
    i = np.array([sx(x & 0x3FF) for x in w], float); q = -np.array([sx(x >> 10 & 0x3FF) for x in w], float)
    i -= i.mean(); q -= q.mean()
    g, p = est(i, q); q2 = (q / g - i * np.sin(p)) / np.cos(p)
    x = i + 1j * q; b = dft(x, -3e6) / np.conj(dft(x, 3e6)); y = x - b * np.conj(x)
    rr = lambda z: 10 * np.log10(tone(z, 3e6) / tone(z, -3e6))
    print('%d MHz: peak %d  image rejection raw %.1f  blind %.1f  tone-beta %.1f dB  (|beta| %.4f arg %.1f deg)' % (mhz, max(abs(i).max(), abs(q).max()), rr(x), rr(i + 1j * q2), rr(y), abs(b), np.degrees(np.angle(b))), flush=True)
tx.cmd('stop')
