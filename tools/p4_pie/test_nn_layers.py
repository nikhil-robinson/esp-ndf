"""nn.c end to end in the model: the same random layers through the PIE build (nn_pie.S) and the portable build of
nn.c; the int8 outputs must be identical (nn.h promises bit-exact TFLite semantics on both)."""
import os, random, shutil, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(__file__))
from sim import Sim
H = os.path.dirname(os.path.abspath(__file__)); C = H + "/../../components"
T = "/tmp/claude-1000/nnlayers"; os.makedirs(T, exist_ok=True)
caps = open(C + "/soc/esp32p4/soc_caps.h").read()
os.makedirs(T + "/c", exist_ok=True)
open(T + "/c/soc_caps.h", "w").write(caps.replace("#define SOC_CPU_HAS_PIE             1", "#define SOC_CPU_HAS_PIE             0"))
TC = open("/tmp/claude-1000/dsp_pie.elf.objdump").read().strip().replace("objdump", "gcc")
def build(out, extra, srcs):
    subprocess.run([TC, "-march=rv32imac_zicsr_zifencei_xespv", "-mabi=ilp32", "-O2", "-mno-relax", "-nostdlib", "-ffreestanding", "-fno-tree-loop-distribute-patterns",
                    "-Wl,-Ttext=0x40000000", "-Wl,--no-warn-rwx-segments", "-Wl,-e,t_conv"] + extra + ["-I" + C + "/include", "-I" + C + "/soc/esp32p4", "-o", out] + srcs + ["-lgcc"], check=True, stderr=subprocess.DEVNULL)
    open(out + ".objdump", "w").write(TC.replace("gcc", "objdump"))
build(T + "/pie.elf", [], [H + "/nn_harness.c", C + "/lib/nn.c", C + "/lib/esp32p4/nn_pie.S"])
build(T + "/c.elf", ["-I" + T + "/c"], [H + "/nn_harness.c", C + "/lib/nn.c"])
rnd = random.Random(11)
s8 = lambda n: bytes(rnd.randrange(256) for _ in range(n))
i32 = lambda *v: struct.pack("<%di" % len(v), *v)

class Run:
    def __init__(self, elf): self.s = Sim(elf, open(elf + ".objdump").read().strip())
    def go(self, fn, args, bufs, outs):
        """args: list of ints or ('buf', i); returns the bytes of the buffers listed in outs"""
        s = self.s; addrs = [s.alloc(b) for b in bufs]
        blk = struct.pack("<%dI" % len(args), *[addrs[a[1]] if isinstance(a, tuple) else a & 0xFFFFFFFF for a in args])
        s.call(fn, s.alloc(blk)); return [s.rd(addrs[i], len(bufs[i])) for i in outs]
pie, ref = Run(T + "/pie.elf"), Run(T + "/c.elf")
def both(fn, args, bufs, outs, tag):
    a, b = pie.go(fn, args, bufs, outs), ref.go(fn, args, bufs, outs)
    assert a == b, (tag, [i for i in range(len(a[0])) if a[0][i] != b[0][i]][:8])

def qparams(oc, rs_lo=1, rs_hi=12):
    return ([rnd.randint(1 << 29, (1 << 31) - 1) for _ in range(oc)], [-rnd.randint(rs_lo, rs_hi) for _ in range(oc)])
for (iw, ih, ic, oc, fh, fw, st, pad) in ((6, 6, 16, 8, 1, 1, 1, 0), (8, 8, 16, 16, 3, 3, 1, 1), (7, 5, 32, 24, 3, 3, 2, 1),
                                          (9, 9, 16, 12, 3, 3, 1, 0), (5, 5, 48, 10, 1, 1, 1, 0)):
    ow, oh = (iw + 2 * pad - fw) // st + 1, (ih + 2 * pad - fh) // st + 1
    mult, shift = qparams(oc); k = fh * fw * ic
    lo, hi = rnd.choice(((-128, 127), (-90, 100)))
    bufs = [i32(iw, ih, ic), s8(iw * ih * ic), s8(oc * k), i32(*[rnd.randint(-3000, 3000) for _ in range(oc)]), i32(ow, oh, oc),
            bytes(ow * oh * oc), i32(rnd.randint(-100, 100), rnd.randint(-20, 20), st, st, pad, pad, lo, hi), i32(*mult), i32(*shift)]
    args = [("b", 0), ("b", 1), fh, fw, ("b", 2), ("b", 3), ("b", 4), ("b", 5), ("b", 6), ("b", 7), ("b", 8)]
    both("t_conv", args, bufs, [5], ("conv", iw, ih, ic, oc, fh, fw))
for (iw, ih, ch, fh, fw, st, pad) in ((8, 8, 16, 3, 3, 1, 1), (9, 7, 32, 3, 3, 2, 1), (6, 6, 16, 5, 5, 1, 2), (8, 8, 16, 3, 3, 1, 0)):
    ow, oh = (iw + 2 * pad - fw) // st + 1, (ih + 2 * pad - fh) // st + 1
    mult, shift = qparams(ch)
    bufs = [i32(iw, ih, ch), s8(iw * ih * ch), s8(fh * fw * ch), i32(*[rnd.randint(-3000, 3000) for _ in range(ch)]), i32(ow, oh, ch),
            bytes(ow * oh * ch), i32(rnd.randint(-100, 100), rnd.randint(-20, 20), st, st, pad, pad, -128, 127), i32(*mult), i32(*shift)]
    both("t_dw", [("b", 0), ("b", 1), fh, fw, ("b", 2), ("b", 3), ("b", 4), ("b", 5), ("b", 6), ("b", 7), ("b", 8)], bufs, [5], ("dw", iw, ih, ch, fh, fw))
for (ln, on, inoff) in ((64, 10, 128), (64, 10, -5), (96, 7, 3), (32, 16, 0)):
    bufs = [s8(ln), s8(ln * on), i32(*[rnd.randint(-2000, 2000) for _ in range(on)]), bytes(on)]
    both("t_fc", [("b", 0), inoff, ln, ("b", 1), rnd.choice((0, 0, 3)), ("b", 2), ("b", 3), on, 4, -9, rnd.randint(1 << 29, (1 << 31) - 1), -128, 127], bufs, [3], ("fc", ln, inoff))
for fn in ("t_max", "t_avg"):
    for (iw, ih, ch) in ((8, 8, 16), (6, 4, 32)):
        ow, oh = iw // 2, ih // 2
        both(fn, [("b", 0), iw, ih, ("b", 1), ow, oh, 2, 2, 2, 2, 0, 0, -128, 127, ch], [s8(iw * ih * ch), bytes(ow * oh * ch)], [1], (fn, iw, ih, ch))
n = 48; both("t_add", [("b", 0), ("b", 1), 7, 9, 1 << 30, 1 << 30, -2, -2, 0, ("b", 2), 5, 1 << 30, -4, -128, 127, n], [s8(n), s8(n), bytes(n)], [2], "add")
both("t_relu6", [("b", 0), 64], [s8(64)], [0], "relu6")
print("nn layers: PIE build == portable build on all cases (%d PIE instructions modelled)" % pie.s.n)
