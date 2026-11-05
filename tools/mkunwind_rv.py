#!/usr/bin/env python3
"""Append an unwind table to an esp-ndf RISC-V app image, for exact panic backtraces (the Xtensa counterpart is mkunwind.py).

  mkunwind_rv.py <readelf> <nm> app.elf app.bin

The RISC-V build keeps no usable CFI (-fno-asynchronous-unwind-tables), so the table is read from the code itself: per
function the instructions that move sp (addi sp,sp,N / c.addi16sp) and save / reload ra (sw ra,off(sp) / c.swsp), which is
all GCC's prologues and epilogues do at -O2. Output rows say, from an address on, the CFA (sp + cfa) and where ra is
(cfa + ra_off, or still in the register). The table is appended to app.bin at the first 256-byte boundary after the ROM
image (the ROM loader ignores it) and found by the panic handler by walking the image header. Format (little endian):
  "NDFV" u16 nrows u16 nirq
  nirq x { u32 start, u32 end }                      interrupt entry code (irq_entry): frame at sp, vectors.S F_*
  nrows x { u32 pc, i16 cfa, i16 ra_off }            sorted by pc; cfa 0x7FFF = no data from here (gap between functions)
  ra_off 0x7FFF: ra is in the register
"""
import re, struct, subprocess, sys

IN_REG = 0x7FFF
NODATA = 0x7FFF


def image_end(img):
    nseg = img[1]
    off = 24
    for _ in range(nseg):
        _, ln = struct.unpack_from('<II', img, off)
        off += 8 + ln
    off = (off + 16) & ~15
    if img[23]:
        off += 32
    return off


def main():
    readelf, nm, elf, binf = sys.argv[1:5]
    objdump = readelf.replace('readelf', 'objdump')
    syms = {}
    for line in subprocess.run([nm, '-S', elf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in 'tTwW':
            syms[p[3]] = (int(p[0], 16), int(p[1], 16))
    dis = subprocess.run([objdump, '-d', '--no-show-raw-insn', elf], capture_output=True, text=True).stdout
    funcs = []                                     # (start, [(addr, text)])
    cur = None
    for line in dis.splitlines():
        m = re.match(r'^([0-9a-f]{8}) <(.+)>:', line)
        if m:
            cur = (int(m.group(1), 16), m.group(2), [])
            funcs.append(cur)
            continue
        m = re.match(r'^\s*([0-9a-f]+):\s+(.*\S)', line)
        if m and cur is not None:
            cur[2].append((int(m.group(1), 16), m.group(2).split('#')[0].strip()))
    rows = []
    for start, name, ins in funcs:
        if name not in syms or not ins:
            continue
        size = syms[name][1] or (ins[-1][0] - start + 4)
        rows.append((start, 0, IN_REG))
        n = 0                                       # frame size once the prologue moved sp
        ra_off = IN_REG                             # where ra lives relative to the CFA once saved
        body = None                                 # state after the prologue
        for i, (addr, text) in enumerate(ins):
            nxt = ins[i + 1][0] if i + 1 < len(ins) else start + size
            m = re.match(r'(?:c\.)?addi(?:16spn|16sp)?\s+sp,(?:sp,)?(-?\d+)$', text)
            if m:
                d = int(m.group(1))
                if d < 0 and n == 0:
                    n = -d
                    rows.append((nxt, n, IN_REG)); body = (n, IN_REG)
                elif d > 0 and n and d == n:
                    rows.append((nxt, 0, IN_REG))   # frame released
                continue
            m = re.match(r'(?:c\.)?sw(?:sp)?\s+ra,(?:(\d+)\(sp\)|(\d+))$', text)
            if m and n:
                off = int(m.group(1) or m.group(2))
                ra_off = off - n
                rows.append((nxt, n, ra_off)); body = (n, ra_off)
                continue
            m = re.match(r'(?:c\.)?lw(?:sp)?\s+ra,(?:(\d+)\(sp\)|(\d+))$', text)
            if m and n:
                rows.append((nxt, n, IN_REG))        # ra back in the register, frame still there
                continue
            if re.match(r'(?:c\.)?(?:ret|jr\s+ra)$', text) and body and i + 1 < len(ins):
                rows.append((nxt, body[0], body[1]))  # code after a return runs with the frame again
        rows.append((start + size, NODATA, 0))
    rows.sort(key=lambda r: (r[0], r[1] != NODATA))
    # keep the last row of an address
    out = []
    for r in rows:
        if out and out[-1][0] == r[0]:
            out[-1] = r
        else:
            out.append(r)
    irq = []
    for s in ('irq_entry',):
        if s in syms:
            irq.append((syms[s][0], syms[s][0] + syms[s][1]))
    table = b'NDFV' + struct.pack('<HH', len(out), len(irq))
    for s, e in irq:
        table += struct.pack('<II', s, e)
    for pc, cfa, ra in out:
        table += struct.pack('<Ihh', pc, cfa if cfa < 0x8000 else NODATA, ra if -0x8000 <= ra < 0x8000 else IN_REG)
    img = bytearray(open(binf, 'rb').read())
    end = image_end(img)
    img = img[:end]
    img += b'\xff' * (((end + 255) & ~255) - end)
    img += table
    open(binf, 'wb').write(img)
    print('unwind table: %d rows, %d bytes in flash' % (len(out), len(table)))


main()
