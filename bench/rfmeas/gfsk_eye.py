#!/usr/bin/env python3
"""TX signal quality: D0 sends 200 B packets (rf_pkt) at power index p; C9:48 captures raw IQ at 16 MS/s (rf_pkt 'iqcap');
FM-demodulate, find the burst, sample the frequency deviation at bit centres. Reports level, the two deviation clusters
(+-250 kHz ideal) and their spread. Raw captures saved to bench/rfmeas/data/. Run from tools/ with PYTHONPATH=."""
import rfhost, numpy as np, sys
def capture(pw):
    tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); tx.cmd('txp %d' % pw)
    tx.cmd('send 2460 0 0x71764129 0 200 0', timeout=10)
    out = rx.cmd('iqcap 2460000', timeout=120)
    hexs = ''.join(l for l in out if not l.startswith('iqcap'))
    w = np.array([int(hexs[k:k + 5], 16) for k in range(0, len(hexs) - 4, 5)], dtype=np.int64)
    i = ((w & 0x3FF) ^ 0x200) - 0x200; q = -((((w >> 10) & 0x3FF) ^ 0x200) - 0x200)
    open('../bench/rfmeas/data/iq_pw%d.txt' % pw, 'w').write(hexs)
    return i + 1j * q
for pw in (11, 8, 5):
    x = capture(pw); p = np.abs(x) ** 2
    env = np.convolve(p, np.ones(64) / 64, 'same'); on = env > env.max() / 4
    f = np.angle(x[1:] * np.conj(x[:-1])) * 16e6 / (2 * np.pi)
    idx = np.where(on[1:])[0]
    if len(idx) < 2000: print('power %d: no burst' % pw); continue
    s0 = idx[0] + 160; s1 = idx[-1] - 160
    fb = np.convolve(f, np.ones(8) / 8, 'same')[s0:s1:16] - np.median(f[s0:s1])
    hi = fb[fb > 0]; lo = fb[fb < 0]
    print('power %2d: burst %d us, level rms %.0f (noise %.0f) | +dev %.0f kHz sd %.0f | -dev %.0f kHz sd %.0f | bits near 0 (<60 kHz): %.1f%%' % (
        pw, len(idx) / 16, np.sqrt(p[on].mean()), np.sqrt(p[~on].mean()), hi.mean() / 1e3, hi.std() / 1e3, lo.mean() / 1e3, lo.std() / 1e3, 100 * np.mean(np.abs(fb) < 60e3)), flush=True)
