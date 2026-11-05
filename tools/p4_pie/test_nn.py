"""Run nn_pie.S in the PIE model against the TFLite-style integer references of components/lib/nn.c."""
import random, struct, subprocess, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from sim import Sim, sat
H = os.path.dirname(os.path.abspath(__file__))
ELF = "/tmp/claude-1000/nn_pie.elf"
subprocess.run([H + "/build.sh", ELF, H + "/../../components/lib/esp32p4/nn_pie.S"], check=True, stderr=subprocess.DEVNULL)
sm = Sim(ELF, open(ELF + ".objdump").read().strip())
rnd = random.Random(7)
def s32(v): return v - (1 << 32) if v >> 31 else v
def rq(acc, mult, rsh, rhalf, negm):
    r = (acc * mult + (1 << 30)) >> 31
    return (r + rhalf + (negm if r < 0 else 0)) >> rsh
def chq(oc):
    l = []
    for _ in range(oc):
        rsh = rnd.randint(1, 12)
        l.append((rnd.randint(-5000, 5000), rnd.randint(1 << 29, (1 << 31) - 1), rsh, 1 << (rsh - 1), -1))
    return l
def pack_q(hdr, ents):
    return struct.pack("<5i", *hdr) + b"".join(struct.pack("<5i", *e) for e in ents)
s8 = lambda n: [rnd.randint(-128, 127) for _ in range(n)]
pk8 = lambda l: struct.pack("<%db" % len(l), *l)
clamp = lambda v, lo, hi: max(lo, min(hi, v))

for off in (0, 1, 7, 15):
    for n16 in (1, 3, 9):
        a, b = s8(16 * n16 + 32), s8(16 * n16)
        ab = sm.alloc(pk8(a)) + off if False else sm.alloc(pk8(a))  # unaligned a via a shifted copy
        ab = sm.alloc(bytes(off) + pk8(a)) + off
        assert s32(sm.call("nn_dot_s8", ab, sm.alloc(pk8(b)), n16)) == sum(x * y for x, y in zip(a, b)), "dot_s8"
for k16, oc in ((1, 5), (2, 4), (3, 7), (9, 16)):
    row, w = s8(16 * k16), s8(16 * k16 * oc); ents = chq(oc)
    oo, lo, hi = rnd.randint(-20, 20), rnd.randint(-128, -60), rnd.randint(60, 127)
    qa = sm.alloc(pack_q((oo, lo, hi, 0, 0), ents)) + 20
    out = sm.alloc(bytes(oc)); sm.call("nn_conv_row_s8", sm.alloc(pk8(row)), k16, sm.alloc(pk8(w)), oc, qa, out)
    ref = [clamp(rq(sum(row[i] * w[o * 16 * k16 + i] for i in range(16 * k16)) + e[0] - e[0], 0, 0, 0, 0) if False else
            rq(sum(row[i] * w[o * 16 * k16 + i] for i in range(16 * k16)) + e[0], e[1], e[2], e[3], e[4]) + oo, lo, hi)
           for o, e in enumerate(ents)]
    assert list(struct.unpack("<%db" % oc, sm.rd(out, oc))) == ref, ("conv_row", k16)
# depthwise
for nt in (1, 9, 25):
    taps_d = [s8(16) for _ in range(nt)]; f = s8(16 * nt)
    tp = [sm.alloc(pk8(t)) for t in taps_d]; tarr = sm.alloc(struct.pack("<%dI" % nt, *tp)); fb = sm.alloc(pk8(f)); raw = sm.alloc(bytes(64))
    sm.call("nn_dw_acc16", tarr, nt, fb, 16, raw)
    acc = [sum(taps_d[t][i] * f[16 * t + i] for t in range(nt)) for i in range(16)]
    assert list(struct.unpack("<16i", sm.rd(raw, 64))) == acc, "dw_acc16"
    ents = chq(16); qa = sm.alloc(pack_q((0, 0, 0, 0, 0), ents)) + 20; out = sm.alloc(bytes(16)); oo = 11
    sm.call("nn_dw_rq16", raw, qa + 0, out, oo)
    assert list(struct.unpack("<16b", sm.rd(out, 16))) == [clamp(rq(acc[i] + ents[i][0], *ents[i][1:]) + oo, -128, 127) for i in range(16)], "dw_rq16"
ch, rb = 32, 32 * 5
img = s8(rb * 3 + 16 * 3 + 64); f = s8(32 * 9)
base = sm.alloc(pk8(img)); raw = sm.alloc(bytes(64)); fb = sm.alloc(pk8(f))
sm.call("nn_dw_acc16_i", base, ch, rb, fb, raw)
acc = [sum(img[y * rb + x * ch + i] * f[(y * 3 + x) * ch + i] for y in range(3) for x in range(3)) for i in range(16)]
assert list(struct.unpack("<16i", sm.rd(raw, 64))) == acc, "dw_acc16_i"
# requant of 32-bit accumulators
n = 21; accs = [rnd.randint(-2**28, 2**28) for _ in range(n)]; ents = chq(n); oo = -9
out = sm.alloc(bytes(n)); sm.call("nn_rq_s8", sm.alloc(struct.pack("<%di" % n, *accs)), n, sm.alloc(pack_q((0,) * 5, ents)) + 20, out, oo)
assert list(struct.unpack("<%db" % n, sm.rd(out, n))) == [clamp(rq(a + e[0], *e[1:]) + oo, -128, 127) for a, e in zip(accs, ents)], "rq_s8"
# fully connected
n32 = 3; x, w = s8(32 * n32 + 32), s8(32 * n32 + 32); sp = [rnd.randint(-128, 127)] * 16
assert s32(sm.call("nn_fc_dot_s8", sm.alloc(pk8(x)), sm.alloc(pk8(w)), n32, sm.alloc(pk8(sp)))) == sum(a * b for a, b in zip(x[:32 * n32], w[:32 * n32])) + sp[0] * sum(w[:32 * n32]), "fc_dot"
v = s8(64); vb = sm.alloc(pk8(v)); sm.call("nn_relu6_v", vb, 4)
assert list(struct.unpack("<64b", sm.rd(vb, 64))) == [clamp(a, 0, 6) for a in v], "relu6"
# pooling helpers
nt = 4; tl = [s8(16) for _ in range(nt)]; tp = [sm.alloc(pk8(t)) for t in tl]; lohi = pk8([-100] * 16 + [90] * 16)
out = sm.alloc(bytes(16)); sm.call("nn_vmax16", sm.alloc(struct.pack("<4I", *tp)), nt, out, sm.alloc(lohi))
assert list(struct.unpack("<16b", sm.rd(out, 16))) == [clamp(max(t[i] for t in tl), -100, 90) for i in range(16)], "vmax16"
cfg = sm.alloc(lohi + bytes(32)); out = sm.alloc(bytes(16)); sm.call("nn_max4_16", *tp, out, cfg)
assert list(struct.unpack("<16b", sm.rd(out, 16))) == [clamp(max(t[i] for t in tl), -100, 90) for i in range(16)], "max4"
out = sm.alloc(bytes(16)); sm.call("nn_avg4_16", *tp, out, cfg)
assert list(struct.unpack("<16b", sm.rd(out, 16))) == [clamp((sum(t[i] for t in tl) + 2 - (1 if sum(t[i] for t in tl) < 0 else 0)) >> 2, -100, 90) for i in range(16)], "avg4"
for k in (0, 1, 3):
    sums = [rnd.randint(-3000, 3000) for _ in range(16)]; raw = sm.alloc(struct.pack("<16i", *sums)); out = sm.alloc(bytes(16))
    sm.call("nn_avg16", raw, out, k, sm.alloc(struct.pack("<2i", -120, 120)))
    half = (1 << k) >> 1
    ref = [clamp((abs(s_) + half >> k) * (1 if s_ >= 0 else -1), -120, 120) for s_ in sums]
    assert list(struct.unpack("<16b", sm.rd(out, 16))) == ref, ("avg16", k)
# add through lookup tables
n = 37; a, b = s8(n), s8(n); ta = [rnd.randint(-2**20, 2**20) for _ in range(256)]; tb = [rnd.randint(-2**20, 2**20) for _ in range(256)]
out = sm.alloc(bytes(n)); mult, rs = rnd.randint(1 << 29, (1 << 31) - 1), 5
k = struct.pack("<8i", out, mult, rs, 1 << (rs - 1), -1, 3, -128, 127)
sm.call("nn_add_lut", sm.alloc(pk8(a)), sm.alloc(pk8(b)), sm.alloc(struct.pack("<256i", *ta)), sm.alloc(struct.pack("<256i", *tb)), n, sm.alloc(k))
assert list(struct.unpack("<%db" % n, sm.rd(out, n))) == [clamp(rq(ta[x & 255] + tb[y & 255], mult, rs, 1 << (rs - 1), -1) + 3, -128, 127) for x, y in zip(a, b)], "add_lut"
print("nn_pie: all kernels match (%d PIE instructions modelled)" % sm.n)
