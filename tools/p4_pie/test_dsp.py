"""Run the dsp_pie.S kernels in the PIE model against plain Python references. Usage: test_dsp.py (builds, then checks)."""
import random, struct, subprocess, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from sim import Sim, sat
H = os.path.dirname(os.path.abspath(__file__))
ELF = "/tmp/claude-1000/dsp_pie.elf"
subprocess.run([H + "/build.sh", ELF, H + "/../../components/lib/esp32p4/dsp_pie.S"], check=True, stderr=subprocess.DEVNULL)
od = open(ELF + ".objdump").read().strip()
sm = Sim(ELF, od)
rnd = random.Random(1)
i16 = lambda n, lo=-32768, hi=32767: [rnd.randint(lo, hi) for _ in range(n)]
pk = lambda l: struct.pack("<%dh" % len(l), *l)

def s32(v): return v - (1 << 32) if v >> 31 else v
for n in (8, 16, 64, 160):
    for mode in ("rand", "max", "min"):
        a = i16(n) if mode == "rand" else [32767 if mode == "max" else -32768] * n
        b = i16(n) if mode == "rand" else [32767 if mode == "max" else 32767] * n
        if mode == "min": b = [32767] * n
        ref = sat(sum(x * y for x, y in zip(a, b)), -2**31, 2**31 - 1)
        got = s32(sm.call("dsp_dot_s16", sm.alloc(pk(a)), sm.alloc(pk(b)), n))
        assert got == ref, ("dot_s16", n, mode, got, ref)
for n in (16, 48):
    a, b, h = i16(n), i16(n), i16(n)
    out = sm.alloc(bytes(8))
    sm.call("dsp_dot2_s16", sm.alloc(pk(a)), sm.alloc(pk(b)), sm.alloc(pk(h)), n, out)
    r0, r1 = (sat(sum(x * y for x, y in zip(v, h)), -2**31, 2**31 - 1) for v in (a, b))
    assert struct.unpack("<ii", sm.rd(out, 8)) == (r0, r1), "dot2"
for n in (8, 16, 48):
    a, b = i16(n, -40000 // 2, 20000), i16(n, -20000, 32767)
    d = sm.alloc(bytes(2 * n))
    sm.call("dsp_add_s16_sat", d, sm.alloc(pk(a)), sm.alloc(pk(b)), n)
    assert list(struct.unpack("<%dh" % n, sm.rd(d, 2 * n))) == [sat(x + y, -32768, 32767) for x, y in zip(a, b)], "add"
src = bytes(rnd.randrange(256) for _ in range(256))
for n in (32, 64, 224):
    d = sm.alloc(bytes(n)); sm.call("dsp_copy128", d, sm.alloc(src), n); assert sm.rd(d, n) == src[:n], "copy128"
pat = bytes(range(16)); d = sm.alloc(bytes(96)); sm.call("dsp_set128", d, sm.alloc(pat), 96); assert sm.rd(d, 96) == pat * 6, "set128"
for off in range(16):
    for n in (48, 96, 144):
        base = sm.alloc(src + src, 16); d = sm.alloc(bytes(n))
        sm.call("dsp_copy128u", d, base + off, n); assert sm.rd(d, n) == (src + src)[off:off + n], ("copy128u", off, n)
def sat16(v): return sat(v, -32768, 32767)
for sh in (0, 1, 5, 15):
    for n8 in (1, 4):
        n = 8 * n8; a, b = i16(n), i16(n)
        d = sm.alloc(bytes(2 * n)); sm.call("dsp_vadds16_sh", sm.alloc(pk(a)), sm.alloc(pk(b)), d, n8, sh)
        assert list(struct.unpack("<%dh" % n, sm.rd(d, 2 * n))) == [sat16(sat16(x + y) >> sh) for x, y in zip(a, b)], ("vadds16_sh", sh)
    n = 32; a, b = i16(n), i16(n)
    d = sm.alloc(bytes(2 * n)); sm.call("dsp_vmuls16_sh", sm.alloc(pk(a)), sm.alloc(pk(b)), d, 2, sh)
    assert list(struct.unpack("<%dh" % n, sm.rd(d, 2 * n))) == [sat16((x * y) >> sh) for x, y in zip(a, b)], ("vmuls16_sh", sh)
for (m, n, k, sh) in ((1, 1, 8, 0), (3, 5, 16, 4), (4, 16, 24, 15), (2, 7, 8, 8)):
    A, B = i16(m * n), i16(n * k)
    C = sm.alloc(bytes(2 * m * k)); sm.call("dsp_mm8_s16", sm.alloc(pk(A)), sm.alloc(pk(B)), C, m, n, k, sh)
    ref = [sat16(sum(A[i * n + p] * B[p * k + j] for p in range(n)) >> sh) for i in range(m) for j in range(k)]
    assert list(struct.unpack("<%dh" % (m * k), sm.rd(C, 2 * m * k))) == ref, ("mm8", m, n, k, sh)
for off in (0, 1, 3, 7):                          # x 2-byte aligned
    g = 3; h = i16(8 * g); x = i16(8 * g + 40)
    xb = sm.alloc(pk(x)) + 2 * off
    ref = sat(sum(x[off + q] * h[q] for q in range(8 * g)), -2**31, 2**31 - 1)
    assert s32(sm.call("dsp_dotu_s16", xb, sm.alloc(pk(h)), g)) == ref, "dotu"
    nout, step, sh = 5, 6, 7; rc = 1 << (sh - 1)
    out = sm.alloc(bytes(2 * nout)); xs = i16(8 * g + 40, -3000, 3000); xb = sm.alloc(pk(xs)) + 2 * off
    sm.call("dsp_fird_core_s16", xb, sm.alloc(pk(h)), out, nout, step, g, sh, rc)
    ref = [sat16((sat(sum(xs[off + i * step // 2 + q] * h[q] for q in range(8 * g)), -2**31, 2**31 - 1) + rc) >> sh) for i in range(nout)]
    assert list(struct.unpack("<%dh" % nout, sm.rd(out, 2 * nout))) == ref, "fird"
for (n, blocks) in ((8, 1), (16, 2), (32, 4)):
    n2 = n // 2 // blocks if False else n // (2 * blocks)
    if n2 < 4: continue
    xs = i16(2 * n, -20000, 20000); w = i16(2 * blocks, -23000, 23000)
    xb = sm.alloc(pk(xs)); sm.call("dsp_fft_s16_stage", xb, n2, blocks, sm.alloc(pk(w)))
    ref = list(xs)
    for j in range(blocks):
        c, s = w[2 * j], w[2 * j + 1]; p0 = 4 * n2 * j
        for i in range(n2):
            a, m = p0 + 2 * i, p0 + 2 * i + 2 * n2
            mr, mi = ref[m], ref[m + 1]
            tr, ti = sat16((c * mr + s * mi) >> 16), sat16((c * mi - s * mr) >> 16)
            hr, hi = (ref[a] * 32767) >> 16, (ref[a + 1] * 32767) >> 16
            ref[a], ref[a + 1], ref[m], ref[m + 1] = sat16(hr + tr), sat16(hi + ti), sat16(hr - tr), sat16(hi - ti)
    assert list(struct.unpack("<%dh" % (2 * n), sm.rd(xb, 4 * n))) == ref, ("fft stage", n, blocks)
print("dsp_pie: all kernels match (%d PIE instructions modelled)" % sm.n)
