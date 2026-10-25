"""Generate components/lib/libm_coef.h: minimax coefficients (tools/remez.py) for the
esp-ndf math library kernels. Prints the achieved relative error of each."""
import sys
import mpmath as mp
from remez import remez
mp.mp.dps = 60
out = []
def emit(name, c, err, ctype):
    fmt = (lambda v: '%.9eF' % float(v)) if ctype == 'float' else (lambda v: '%.17e' % float(v))
    out.append('static const %s %s[%d] = { %s };   /* max rel err %.2e */' % (ctype, name, len(c), ', '.join(fmt(v) for v in c), float(err)))
    print('%-14s n=%d err=%.3e' % (name, len(c) - 1, float(err)), file=sys.stderr)

def fit(name, f, w, a, b, n, ctype):
    c, e = remez(f, w, a, b, n)
    emit(name, c, e, ctype)

def series_safe(fn, t0, fs):
    """f(t) with the removable singularity at 0 filled by its series value"""
    return lambda t: fs if t == 0 else fn(t)

PI4 = mp.pi / 4
# ---------------- float kernels ----------------
# sin(x) = x + x t S(t), t = x^2 <= (pi/4)^2 ; relative error of sin ~ t (S - P)
S = lambda t: (mp.sin(mp.sqrt(t)) / mp.sqrt(t) - 1) / t if t else mp.mpf(-1) / 6
fit('kSinF', S, lambda t: t if t else mp.mpf(10) ** -30, 0, PI4 ** 2, 2, 'float')
# cos(x) = 1 - t/2 + t^2 C(t)
C = lambda t: (mp.cos(mp.sqrt(t)) - 1 + t / 2) / t ** 2 if t else mp.mpf(1) / 24
fit('kCosF', C, lambda t: t * t / mp.cos(mp.sqrt(t)) if t else mp.mpf(10) ** -30, 0, PI4 ** 2, 2, 'float')
# tan(x) = x + x t T(t)
T = lambda t: (mp.tan(mp.sqrt(t)) / mp.sqrt(t) - 1) / t if t else mp.mpf(1) / 3
fit('kTanF', T, lambda t: t * mp.sqrt(t) / mp.tan(mp.sqrt(t)) if t else mp.mpf(10) ** -30, 0, PI4 ** 2, 6, 'float')
# exp(r) = 1 + r + r^2 E(r), |r| <= ln2/64
L64 = mp.log(2) / 64
E = lambda r: (mp.exp(r) - 1 - r) / r ** 2 if r else mp.mpf(1) / 2
fit('kExpF', E, lambda r: r * r / mp.exp(r) if r else mp.mpf(10) ** -30, -L64, L64, 1, 'float')
# log1p(r) = r - r^2/2 + r^3 L(r), |r| <= 1/64 (table reduction)
Lf = lambda r: (mp.log1p(r) - r + r * r / 2) / r ** 3 if r else mp.mpf(1) / 3
fit('kLogF', Lf, lambda r: abs(r ** 3 / mp.log1p(r)) if r else mp.mpf(10) ** -30, -mp.mpf('0.0205'), mp.mpf('0.0315'), 2, 'float')
# atan(u) = u + u t A(t), |u| <= tan(pi/8)
T8 = mp.tan(mp.pi / 8)
A = lambda t: (mp.atan(mp.sqrt(t)) / mp.sqrt(t) - 1) / t if t else mp.mpf(-1) / 3
fit('kAtanF', A, lambda t: t * mp.sqrt(t) / mp.atan(mp.sqrt(t)) if t else mp.mpf(10) ** -30, 0, T8 ** 2, 4, 'float')
# asin(x) = x + x t B(t), t = x^2 <= 1/4
B = lambda t: (mp.asin(mp.sqrt(t)) / mp.sqrt(t) - 1) / t if t else mp.mpf(1) / 6
fit('kAsinF', B, lambda t: t * mp.sqrt(t) / mp.asin(mp.sqrt(t)) if t else mp.mpf(10) ** -30, 0, mp.mpf(1) / 4, 5, 'float')
# expm1(x) = x + x^2 M(x), |x| <= ln2/2
M = lambda x: (mp.expm1(x) - x) / x ** 2 if x else mp.mpf(1) / 2
fit('kExpm1F', M, lambda x: abs(x * x / mp.expm1(x)) if x else mp.mpf(10) ** -30, -mp.log(2) / 2, mp.log(2) / 2, 5, 'float')

# odd kernels f(x) = x + x t K(t), t = x^2
def oddk(name, fn, tmax, n, c0):
    K = lambda t: (fn(mp.sqrt(t)) / mp.sqrt(t) - 1) / t if t else mp.mpf(c0)
    fit(name, K, lambda t: abs(t * mp.sqrt(t) / fn(mp.sqrt(t))) if t else mp.mpf(10) ** -30, 0, tmax, n, 'float')
oddk('kAtan1F', mp.atan, 1, 9, -mp.mpf(1) / 3)   # atan on [0, 1], no reduction
oddk('kSinhF', mp.sinh, 1, 4, mp.mpf(1) / 6)
oddk('kTanhF', mp.tanh, mp.mpf('0.3025'), 6, -mp.mpf(1) / 3)
oddk('kAsinhF', mp.asinh, mp.mpf('0.25'), 6, -mp.mpf(1) / 6)
oddk('kAtanhF', mp.atanh, mp.mpf('0.25'), 6, mp.mpf(1) / 3)
# ---------------- tables ----------------
import struct
def f32(v): return struct.unpack('<f', struct.pack('<f', float(v)))[0]
def f32bits(v): return struct.unpack('<I', struct.pack('<f', float(v)))[0]
# 2^(j/32) as hi + lo floats
hi = [f32(mp.mpf(2) ** (mp.mpf(j) / 32)) for j in range(32)]
lo = [f32(mp.mpf(2) ** (mp.mpf(j) / 32) - mp.mpf(hi[j])) for j in range(32)]
out.append('static const float kExp2TabHi[32] = { %s };' % ', '.join('%.9eF' % v for v in hi))
out.append('static const float kExp2TabLo[32] = { %s };' % ', '.join('%.9eF' % v for v in lo))
# log: for m in [1, 2) interval j = [1 + j/32, 1 + (j+1)/32); j >= 13 is used as m/2 with e + 1.
# invc: 9 significant bits (exact integer reduction r = m * invc - 1); j = 0 -> 1, j = 31 -> 1/2.
inv9, lch, lcl = [], [], []
for j in range(32):
    c = 1 + (mp.mpf(j) + mp.mpf(0.5)) / 32
    if j >= 13: c = c / 2
    iv = 1 / c
    if j == 0: iv = mp.mpf(1)
    if j == 31: iv = mp.mpf(1)        # m/2 in [0.984, 1): reciprocal of 1 (r = m/2 - 1)
    q = int(mp.nint(iv * 256))                # multiple of 1/256: stored exactly as q
    ivq = mp.mpf(q) / 256
    inv9.append(int(ivq * 256))               # invc * 256 as an integer (exact: invc has <= 9 bits, >= 2^-1)
    lc = -mp.log(ivq)                         # log(c) for c = 1/invc
    h = mp.nint(lc * 2 ** 16) / 2 ** 16       # multiple of 2^-16: e*ln2hi + logc_hi exact
    lch.append(f32(h)); lcl.append(f32(lc - h))
out.append('static const uint16_t kLogInv[32] = { %s };   /* (1/c) * 256, exact */' % ', '.join(str(v) for v in inv9))
out.append('static const float kLogCHi[32] = { %s };' % ', '.join('%.9eF' % v for v in lch))
out.append('static const float kLogCLo[32] = { %s };' % ', '.join('%.9eF' % v for v in lcl))
# 2/pi bits, 32 per word, most significant first: 2/pi = 0.w0 w1 w2 ...
v = 2 / mp.pi
mp.mp.dps = 400
v = 2 / mp.pi
words = []
for i in range(48):
    v *= 2 ** 32
    w = int(mp.floor(v)); words.append(w); v -= w
mp.mp.dps = 60
out.append('static const uint32_t kTwoOverPi[48] = { %s };' % ', '.join('0x%08X' % w for w in words))

# float constants: hi = float(v) (or rounded to k bits), lo = float(v - hi)
mp.mp.dps = 60
def split(name, v, bits=None):
    v = mp.mpf(v)
    if bits:
        e = mp.floor(mp.log(abs(v), 2))
        h = mp.nint(v * 2 ** (bits - 1 - e)) / 2 ** (bits - 1 - e)
    else:
        h = mp.mpf(f32(v))
    l = f32(v - h)
    out.append('#define %s_HI %.9eF\n#define %s_LO %.9eF' % (name, f32(h), name, l))
def one(name, v): out.append('#define %s %.9eF' % (name, f32(v)))
split('CF_LN2_32', mp.log(2) / 32)
one('CF_INV_LN2_32', 32 / mp.log(2))
split('CF_LN2', mp.log(2))                 # full float hi (exp2: f * ln2 as a pair)
split('CF_LN2_16', mp.log(2), 16)          # 16-bit hi: e * hi exact (logs, expm1)
one('CF_INV_LN2', 1 / mp.log(2))
split('CF_INVLN2', 1 / mp.log(2))
split('CF_INVLN10', 1 / mp.log(10))
split('CF_LOG10_2', mp.log10(2), 15)
split('CF_LN10', mp.log(10))
one('CF_32LOG2_10', 32 * mp.log(10, 2))
one('CF_2OVERPI', 2 / mp.pi)
c1 = mp.mpf(f32(mp.pi / 2)); c2 = mp.mpf(f32(mp.pi / 2 - c1)); c3 = mp.mpf(f32(mp.pi / 2 - c1 - c2))
out.append('#define CF_PIO2_1 %.9eF\n#define CF_PIO2_2 %.9eF\n#define CF_PIO2_3 %.9eF   /* sum: %.1f bits */' % (c1, c2, c3, float(-mp.log(abs(mp.pi / 2 - c1 - c2 - c3) / (mp.pi / 2), 2))))
split('CF_PIO2_64', mp.pi / 2 * mp.mpf(2) ** -64)
split('CF_PIO2', mp.pi / 2)
split('CF_PIO4', mp.pi / 4)
split('CF_PI', mp.pi)
one('CF_LN2_F', mp.log(2))
one('CF_25LOG10_2', 25 * mp.log10(2))
print('/* generated by tools/gen_libm.py (minimax, tools/remez.py) */')
print('\n'.join(out))
