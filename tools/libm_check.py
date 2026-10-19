#!/usr/bin/env python3
"""Accuracy report for examples/libm_test: reads the board's hex results
(stdin or file), regenerates the same inputs (same xorshift + generators),
computes exact values with mpmath and prints, per function, the largest
error in ulps and the share of correctly rounded results.
  python3 tools/libm_check.py log.txt"""
import struct, sys, re
import mpmath as mp
mp.mp.dps = 50

def f32(u): return struct.unpack('<f', struct.pack('<I', u))[0]
def u32(f): return struct.unpack('<I', struct.pack('<f', f))[0]

class Rng:
    def __init__(self, s): self.s = s & 0xFFFFFFFF
    def next(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF; x ^= x >> 17; x ^= (x << 5) & 0xFFFFFFFF
        self.s = x; return x

def gen(r, emin, emax, neg):
    e = emin + r.next() % (emax - emin + 1) + 127
    u = (e << 23) | (r.next() & 0x7FFFFF)
    if neg and (r.next() & 1): u |= 0x80000000
    return f32(u)

def fl(v):                                     # float32 rounding of a python float (round to nearest)
    return struct.unpack('<f', struct.pack('<f', v))[0]

def bounded(r, lo, hi):
    lo, hi = fl(lo), fl(hi)
    t = fl(fl((r.next() >> 8) * 2.0 ** -24) * fl(hi - lo))   # the device: lo + (hi - lo) * frac, float ops
    return fl(lo + t)

M = mp.mpf
from fractions import Fraction
def cfmod(a, b):
    fa, fb = Fraction(float(a)), Fraction(float(b))
    q = fa / fb
    t = q.numerator // q.denominator if q >= 0 else -((-q.numerator) // q.denominator)
    r = fa - t * fb
    return M(r.numerator) / r.denominator if r else (M(0) if fa >= 0 else -M(0))
FN = {   # name: (exact function, input spec) ; spec mirrors the device table
    'sinf': (mp.sin, ('g', -20, 8, 1)), 'sinf_big': (mp.sin, ('g', 9, 127, 1)),
    'cosf': (mp.cos, ('g', -20, 8, 1)), 'cosf_big': (mp.cos, ('g', 9, 127, 1)),
    'tanf': (mp.tan, ('g', -20, 8, 1)), 'tanf_big': (mp.tan, ('g', 9, 127, 1)),
    'asinf': (mp.asin, ('b', -1.0, 1.0)), 'acosf': (mp.acos, ('b', -1.0, 1.0)),
    'atanf': (mp.atan, ('g', -20, 30, 1)), 'atan2f': (lambda a, b: mp.atan2(a, b), ('g2', -20, 20, 1, -20, 20)),
    'expf': (mp.exp, ('b', -103.0, 88.7)), 'exp2f': (lambda x: M(2) ** x, ('b', -149.0, 127.9)),
    'exp10f': (lambda x: M(10) ** x, ('b', -45.0, 38.5)), 'expm1f': (mp.expm1, ('g', -26, 6, 1)),
    'logf': (mp.log, ('g', -126, 127, 0)), 'log2f': (lambda x: mp.log(x, 2), ('g', -126, 127, 0)),
    'log10f': (mp.log10, ('g', -126, 127, 0)), 'log1pf': (mp.log1p, ('g', -26, 30, 0)),
    'logf_near1': (mp.log, ('b', 0.9, 1.1)),
    'powf': (lambda a, b: M(a) ** b, ('pow', 0.01, 100.0, -8, 4)),
    'cbrtf': (mp.cbrt, ('g', -126, 127, 1)), 'hypotf': (lambda a, b: mp.hypot(a, b), ('g2', -60, 60, 1, -60, 60)),
    'sinhf': (mp.sinh, ('g', -20, 6, 1)), 'coshf': (mp.cosh, ('g', -20, 6, 1)), 'tanhf': (mp.tanh, ('g', -20, 4, 1)),
    'asinhf': (mp.asinh, ('g', -20, 40, 1)), 'acoshf': (mp.acosh, ('b', 1.0, 50.0)), 'atanhf': (mp.atanh, ('b', -0.999, 0.999)),
    'sqrtf': (mp.sqrt, ('g', -126, 127, 0)), 'fmodf': (lambda a, b: cfmod(a, b), ('g2', -20, 30, 1, -20, 20)),
}
ORDER = list(FN)

def inputs(k, name, n):
    spec = FN[name][1]
    r = Rng(0x9E3779B9 ^ ((k * 0x01000193) & 0xFFFFFFFF))
    out = []
    for _ in range(n):
        if spec[0] == 'g': out.append((gen(r, *spec[1:4]),))
        elif spec[0] == 'b': out.append((bounded(r, spec[1], spec[2]),))
        elif spec[0] == 'g2':
            x = gen(r, spec[1], spec[2], spec[3]); y = gen(r, spec[4], spec[5], 1); out.append((x, y))
        elif spec[0] == 'pow':
            x = bounded(r, spec[1], spec[2]); y = bounded(r, float(spec[3]), float(spec[4])); out.append((x, y))
    return out

def ulp_err(res, exact):
    """error of float result vs exact mpf, in ulps of the exact value's binade"""
    if exact == 0: return 0.0 if res == 0 else float('inf')
    e = int(mp.floor(mp.log(abs(exact), 2)))
    e = max(e, -126)
    ulp = M(2) ** (e - 23)
    if abs(exact) >= M(2) ** 128 - M(2) ** 103:      # rounds to inf
        return 0.0 if res in (float('inf'), float('-inf')) and (res > 0) == (exact > 0) else float('inf')
    if res != res or res in (float('inf'), float('-inf')): return float('inf')
    return float(abs(M(res) - exact) / ulp)

def main():
    txt = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
    blocks = re.split(r'^@', txt, flags=re.M)[1:]
    print('%-12s %6s %9s %8s %8s' % ('function', 'n', 'max ulp', 'exact %', '> 1 ulp'))
    worst_all = 0
    for b in blocks:
        head, _, body = b.partition('\n')
        name, n = head.split()[0], int(head.split()[1])
        res = [f32(int(h, 16)) for h in re.findall(r'\b[0-9a-f]{8}\b', body.split('@')[0])][:n]
        k = ORDER.index(name)
        ins = inputs(k, name, n)
        fn = FN[name][0]
        worst, worst_in, exact_cnt, bad = 0.0, None, 0, 0
        for a, r in zip(ins, res):
            try:
                ex = fn(*[M(v) for v in a])
            except (ValueError, ZeroDivisionError):
                continue
            if isinstance(ex, mp.mpc) or ex != ex: continue
            err = ulp_err(r, ex)
            if err <= 0.5 + 1e-9: exact_cnt += 1
            if err > 1.0: bad += 1
            if err > worst: worst, worst_in = err, (a, r, ex)
        worst_all = max(worst_all, worst)
        print('%-12s %6d %9.3f %7.1f%% %8d' % (name, len(res), worst, 100.0 * exact_cnt / max(1, len(res)), bad) +
              ('   worst at %s -> %r (exact %s)' % (worst_in[0], worst_in[1], mp.nstr(worst_in[2], 12)) if worst > 1.0 else ''))
    print('largest error: %.3f ulp' % worst_all)

main()
