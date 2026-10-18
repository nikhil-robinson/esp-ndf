#!/usr/bin/env python3
"""Append an unwind table to an esp-ndf app image, for exact panic backtraces.

  mkunwind.py <readelf> <nm> app.elf app.bin

The table comes from the ELF's .debug_frame (DWARF CFI that -g emits),
interpreted by readelf -wF, reduced to what a CALL0 unwinder needs per
address range: the CFA rule (a1 or a15 + offset) and where the return
address (a0) and a15 are saved. It is appended to app.bin at the first
256-byte boundary after the ROM image (the ROM loader ignores it), so it
costs flash, not RAM. The panic handler finds it by walking the image
header. Format (little endian):
  "NDFU" u16 nfde u16 nirq
  nirq x { u32 start, u32 end, u32 level }        interrupt entry code ranges
  nfde x { u32 start, u32 end, u32 rows, u32 nrows }   sorted by start; rows = byte offset
  rows: { u16 pc_off, u8 cfa_reg, u8 0, u16 cfa_off, i16 ra_off, i16 a15_off }  (0x7FFF: in register)
"""
import re, struct, subprocess, sys

IN_REG = 0x7FFF


def image_end(img):
    """length of the ROM image: header, segments, checksum block, optional SHA-256"""
    nseg = img[1]
    off = 24
    for _ in range(nseg):
        _, ln = struct.unpack_from('<II', img, off)
        off += 8 + ln
    off = (off + 16) & ~15                       # checksum: last byte of a 16-byte block
    if img[23]:                                  # hash appended
        off += 32
    return off


def main():
    readelf, nm, elf, binf = sys.argv[1:5]
    cfi = subprocess.run([readelf, '-wF', elf], capture_output=True, text=True).stdout
    fdes = []
    cur = None
    cols = []
    for line in cfi.splitlines():
        if ' CIE ' in line:
            cur = None                           # CIE rows are not a function's
            continue
        m = re.search(r'FDE cie=\w+ pc=([0-9a-f]+)\.\.([0-9a-f]+)', line)
        if m:
            s, e = int(m.group(1), 16), int(m.group(2), 16)
            cur = {'start': s, 'end': e, 'rows': []} if s else None
            if cur:
                fdes.append(cur)
            cols = []
            continue
        if cur is None:
            continue
        if line.strip().startswith('LOC'):
            cols = line.split()
            continue
        parts = line.split()
        if not parts or not cols or not re.fullmatch(r'[0-9a-f]{8}', parts[0]):
            continue
        row = dict(zip(cols, parts))
        loc = int(parts[0], 16)
        if not cur['start'] <= loc < cur['end']:
            continue
        cm = re.fullmatch(r'r(\d+)\+(\d+)', row.get('CFA', ''))
        if not cm:
            continue                             # expression CFA: not produced by gcc for CALL0
        def slot(name):
            v = row.get(name, 'u')
            mm = re.fullmatch(r'c(-?\d+)', v)
            return int(mm.group(1)) if mm else IN_REG
        cur['rows'].append((loc - cur['start'], int(cm.group(1)), int(cm.group(2)), slot('ra'), slot('r15')))
    fdes = [f for f in fdes if f['rows']]
    fdes.sort(key=lambda f: f['start'])

    syms = {}
    for line in subprocess.run([nm, elf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    order = ['_l1_entry', '_l1_exception', '_l2_entry', '_l3_entry', '_l4_entry', '_l5_entry', '_double']
    irq = []
    if all(s in syms for s in order):
        irq = [(syms['_l1_entry'], syms['_l1_exception'], 1), (syms['_l2_entry'], syms['_l3_entry'], 2),
               (syms['_l3_entry'], syms['_l4_entry'], 3), (syms['_l4_entry'], syms['_l5_entry'], 4),
               (syms['_l5_entry'], syms['_double'], 5)]

    rows = b''
    index = b''
    for f in fdes:
        index += struct.pack('<IIII', f['start'], f['end'], len(rows), len(f['rows']))
        for r in f['rows']:
            rows += struct.pack('<HBBHhh', r[0], r[1], 0, r[2], r[3], r[4])
    head = b'NDFU' + struct.pack('<HH', len(fdes), len(irq))
    for s, e, l in irq:
        head += struct.pack('<III', s, e, l)
    table = head + index + rows

    img = bytearray(open(binf, 'rb').read())
    end = image_end(img)
    img = img[:end]                              # drop an older table
    img += b'\xff' * (((end + 255) & ~255) - end)
    img += table
    open(binf, 'wb').write(img)
    print('unwind table: %d functions, %d rows, %d bytes in flash' % (len(fdes), len(rows) // 10, len(table)))


main()
