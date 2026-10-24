#!/usr/bin/env python3
"""SPIFFS image builder, same on-flash format as esp-ndf drivers/storage/spiffs.c and ESP-IDF's spiffs at its default
config (256 B pages, 4 KB blocks, 32 B names, 4 B meta, magic + length). Flat names = paths relative to DIR.
  spiffsgen.py DIR out.bin SIZE"""
import os, struct, sys

PG, BLK = 256, 4096
PPB, ENT, DATA = 16, 15, 251
NAME, N_IX0, N_IX, HDR_IX = 32, 103, 124, 49
IXF, FREE = 0x8000, 0xFFFF
FL_INDEX, FL_DATA = 0xF8, 0xFC


def build(src, size):
    if size % BLK:
        sys.exit('size must be a multiple of 4096')
    nblk = size // BLK
    img = bytearray(b'\xff' * size)
    lu = [[FREE] * ENT for _ in range(nblk)]
    nxt = [0]                                     # next free page, counted over usable pages only

    def put(page, luv):
        n = nxt[0]
        b, e = divmod(n, ENT)
        if b >= nblk - 2:                         # leave the 2 spare blocks the driver keeps for GC
            sys.exit('image full')
        pix = b * PPB + 1 + e
        img[pix * PG:(pix + 1) * PG] = page
        lu[b][e] = luv
        nxt[0] += 1
        return pix

    def hdr(oid, span, fl):
        return bytearray(struct.pack('<HHB', oid, span, fl)) + b'\xff' * (PG - 5)

    files = []
    for root, _, names in os.walk(src):
        for n in sorted(names):
            full = os.path.join(root, n)
            files.append(('/' + os.path.relpath(full, src).replace(os.sep, '/'), open(full, 'rb').read()))
    for oid, (name, data) in enumerate(sorted(files), 1):
        nb = name.encode()
        if len(nb) >= NAME:
            sys.exit('name too long (max 31): ' + name)
        refs = []
        for span in range((len(data) + DATA - 1) // DATA):
            p = hdr(oid, span, FL_DATA)
            chunk = data[span * DATA:(span + 1) * DATA]
            p[5:5 + len(chunk)] = chunk
            refs.append(put(p, oid))
        h = hdr(oid | IXF, 0, FL_INDEX)
        h[8:12] = struct.pack('<I', len(data)); h[12] = 1
        h[13:13 + NAME + 4] = bytes(NAME + 4); h[13:13 + len(nb)] = nb
        for i, r in enumerate(refs[:N_IX0]):
            struct.pack_into('<H', h, HDR_IX + 2 * i, r)
        put(h, oid | IXF)
        rest = refs[N_IX0:]
        for k in range(1, 1 + (len(rest) + N_IX - 1) // N_IX):
            x = hdr(oid | IXF, k, FL_INDEX)
            for i, r in enumerate(rest[(k - 1) * N_IX:k * N_IX]):
                struct.pack_into('<H', x, 8 + 2 * i, r)
            put(x, oid | IXF)
    for b in range(nblk):                         # lookup page: object ids, magic, erase count 0
        o = b * BLK
        for e in range(ENT):
            struct.pack_into('<H', img, o + 2 * e, lu[b][e])
        struct.pack_into('<HH', img, o + PG - 4, (0x20140529 ^ PG ^ (nblk - b)) & 0xFFFF, 0)
    return bytes(img)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    open(sys.argv[2], 'wb').write(build(sys.argv[1], int(sys.argv[3], 0)))
