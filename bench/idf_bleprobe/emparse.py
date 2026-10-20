#!/usr/bin/env python3
"""Parse a probe dump: segments (EM offset -> DRAM) and the 8 KB EM snapshot; helpers to read EM by offset."""
import re, struct, sys
def load(path):
    t = open(path).read()
    segs = [(int(a, 16), int(b, 16)) for a, b in re.findall(r'^seg off (\w+) addr (\w+)', t, re.M)]
    base = int(re.search(r'emdump base (\w+)', t).group(1), 16)
    mem = bytearray()
    for l in re.findall(r'^m \w+ (.*)$', t, re.M):
        for w in l.split(): mem += struct.pack('<I', int(w, 16))
    core = {int(a, 16): int(v, 16) for a, v in re.findall(r'^c (\w+) (\w+)', t, re.M)}
    return segs, base, bytes(mem), core
def em_addr(segs, off):
    s = max((s for s in segs if s[0] <= off), key=lambda s: s[0]); return s[1] + off - s[0]
def em_off(segs, addr):
    c = [(o, a) for o, a in segs if a <= addr]; o, a = max(c, key=lambda s: s[1]); return o + addr - a
if __name__ == '__main__':
    segs, base, mem, core = load(sys.argv[1])
    pat = bytes.fromhex(sys.argv[2]) if len(sys.argv) > 2 else bytes([1, 2, 3, 0xd, 0xf, 0xc0])
    for m in re.finditer(re.escape(pat), mem):
        a = base + m.start(); print('found at %08x em off %04x' % (a, em_off(segs, a)), mem[m.start() - 4:m.start() + 20].hex())
