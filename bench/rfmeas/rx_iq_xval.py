#!/usr/bin/env python3
"""RX IQ correction, cross-validated: beta from capture A (tone at +3 MHz baseband), applied to capture B (same) and
C (tone at +5 MHz); also blind moments from A applied to B / C. Raw vs corrected image rejection.
Run from tools/ with PYTHONPATH=."""
import rfhost, numpy as np, os
d = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')
sx = lambda v: v - 1024 if v & 512 else v
dft = lambda x, f: np.sum(x * np.exp(-2j * np.pi * f * np.arange(len(x)) / 16e6))
rr = lambda z, f: 10 * np.log10(abs(dft(z, f)) ** 2 / abs(dft(z, -f)) ** 2)
def cap(khz, tag):
    lines = rx.cmd('iqcap %d 200' % khz, timeout=60)[1:]
    open(os.path.join(d, 'rx_iq_%s.txt' % tag), 'w').write('\n'.join(lines) + '\n')
    w = [int(l[k:k + 5], 16) for l in lines for k in range(0, len(l), 5)]
    x = np.array([sx(v & 0x3FF) for v in w], float) - 1j * np.array([sx(v >> 10 & 0x3FF) for v in w], float)
    return x - x.mean()
def blind(x):
    i, q = x.real, x.imag; ii, qq, iq = np.mean(i * i), np.mean(q * q), np.mean(i * q)
    g, p = np.sqrt(qq / ii), np.arcsin(iq / np.sqrt(ii * qq)); return g, p
def blind_apply(x, gp): g, p = gp; i, q = x.real, x.imag; return i + 1j * (q / g - i * np.sin(p)) / np.cos(p)
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948)
for mhz in (2404, 2440, 2478):
    tx.cmd('tone %d 1000' % (mhz * 1000), timeout=10)
    A = cap((mhz - 2) * 1000, '%d_a' % mhz); B = cap((mhz - 2) * 1000, '%d_b' % mhz); C = cap((mhz - 4) * 1000, '%d_c' % mhz)
    b = dft(A, -3e6) / np.conj(dft(A, 3e6)); gp = blind(A)
    for name, X, f in (('B +3', B, 3e6), ('C +5', C, 5e6)):
        print('%d %s MHz: raw %.1f  beta(A) %.1f  blind(A) %.1f dB' % (mhz, name, rr(X, f), rr(X - b * np.conj(X), f), rr(blind_apply(X, gp), f)), flush=True)
tx.cmd('stop')
