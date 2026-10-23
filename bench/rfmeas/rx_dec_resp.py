#!/usr/bin/env python3
"""rf_rx_capture_dec response: D0 tone at 2440 MHz + f, C9:48 deccap (decim 8 = 2 MS/s 48 taps, 16 = 1 MS/s 128 taps, 512
samples, 20 dB gain); measured vs the theoretical response of the same taps; tone power at its (aliased) output frequency, dB relative to f = 100 kHz. Then a long run into PSRAM (rc, late).
Run from tools/ with PYTHONPATH=."""
import rfhost, numpy as np
OFFS = {8: [100, 300, 500, 600, 750, 900, 1100, 1300, 1500, 2300, 3300, 5300], 16: [100, 200, 300, 400, 500, 650, 800, 1150, 1300, 2300, 3300, 5300]}   # none alias onto DC (LO leak)
def cap(D):
    r = rx.cmd('deccap 2440000 %d 512 20' % D, timeout=30)
    s = [int(l[k:k + 8], 16) for l in r[1:] for k in range(0, len(l), 8)]
    sx = lambda v: v - 65536 if v & 32768 else v
    return r[0], np.array([sx(v >> 16) + 1j * sx(v & 0xFFFF) for v in s], complex)
def pw(x, f, fs):
    fa = (f + fs / 2) % fs - fs / 2; n = np.arange(len(x)); w = np.hanning(len(x))
    return 10 * np.log10(abs(np.sum(w * x * np.exp(-2j * np.pi * fa * n / fs))) ** 2 + 1e-9)
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948)
for D in (8, 16):
    fs = 16e6 / D; r = []; offs = OFFS[D]; T = 48 if D == 8 else 128
    fc = 0.75 / D / 2; k = np.arange(T) - (T - 1) / 2; h = np.sinc(2 * fc * k) * 2 * fc * np.hamming(T); h /= h.sum()
    th = lambda f: 20 * np.log10(abs(np.sum(h * np.exp(-2j * np.pi * f / 16e6 * np.arange(T)))) + 1e-12)
    for f in offs:
        tx.cmd('tone 2440000 %d' % f, timeout=10); hdr, x = cap(D); r.append(pw(x, f * 1e3, fs))
    print('decim %d (%.0f MS/s, %d taps) %s:' % (D, fs / 1e6, T, hdr), flush=True)
    for f, v in zip(offs, r): print('   %5d kHz  measured %6.1f  theory %6.1f dB' % (f, v - r[0], th(f * 1e3) - th(1e5)), flush=True)
tx.cmd('stop')
for D in (8, 16):
    print('long run decim %d, 400000 samples:' % D, rx.cmd('deccap 2440000 %d 400000 20' % D, timeout=60)[0], flush=True)
