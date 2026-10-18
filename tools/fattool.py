#!/usr/bin/env python3
"""FAT image tool: build a FAT12/16 image (long names) from a directory, and
list / extract any FAT12/16/32 image. Optional wear-levelling wrapper in the
ESP-IDF WL v2 format, so the image mounts with esp-ndf wl_mount() and IDF's
esp_vfs_fat_spiflash_mount_rw_wl(). Geometry rules match esp-ndf fat_format().

  fattool.py mkfat DIR out.bin SIZE [--wl] [--sector 4096]
  fattool.py parse image.bin [--wl] [-x OUTDIR]
"""
import argparse, os, struct, sys, time, zlib

S_DEFAULT = 4096
WL_REC = 16


# ---- wear levelling (IDF v2) --------------------------------------------
def wl_geometry(size, S=4096):
    state = -(-(64 + size // S * WL_REC) // S) * S
    cfg_addr = size - S
    st1 = cfg_addr - 2 * state
    return state, st1, cfg_addr, (st1 // S - 1) * S          # flash_size: minus the dummy


def crc(b):
    return zlib.crc32(b, 0xFFFFFFFF)


def wl_wrap(logical, size, S=4096):
    """fresh WL volume: dummy at sector 0, move count 0 -> logical l at phys l + 1"""
    state, st1, cfg_addr, fs = wl_geometry(size, S)
    assert len(logical) == fs
    img = bytearray(b'\xff' * size)
    img[S:S + fs] = logical
    st = struct.pack('<8I', 0, 1 + fs // S, 0, 0, 16, S, 2, int.from_bytes(os.urandom(4), 'little')) + b'\0' * 28
    st += struct.pack('<I', crc(st))
    img[st1:st1 + 64] = st
    img[st1 + state:st1 + state + 64] = st
    cfg = struct.pack('<8I', 0, size, S, S, 16, WL_REC, 2, 32)
    img[cfg_addr:cfg_addr + 48] = cfg + struct.pack('<I', crc(cfg)) + b'\xff' * 12
    return bytes(img)


def wl_unwrap(img, S=4096):
    size = len(img)
    state, st1, _, fs = wl_geometry(size, S)
    w = struct.unpack_from('<16I', img, st1)
    if crc(img[st1:st1 + 60]) != w[15]:
        w = struct.unpack_from('<16I', img, st1 + state)        # state 2 copy
        if crc(img[st1 + state:st1 + state + 60]) != w[15]:
            sys.exit('no valid WL state')
    max_pos, move, dev = w[1], w[2], w[7]
    pos = 0
    while pos < max_pos:
        exp = b''.join(struct.pack('<I', crc(struct.pack('<I', (dev + pos * 4 + i) & 0xFFFFFFFF))) for i in range(4))
        if img[st1 + 64 + pos * WL_REC:st1 + 80 + pos * WL_REC] != exp:
            break
        pos += 1
    dummy = pos - 1 if pos == max_pos else pos
    out = bytearray(fs)
    for l in range(fs // S):
        a = (fs - move * S + l * S) % fs
        a = a if a < dummy * S else a + S
        out[l * S:(l + 1) * S] = img[a:a + S]
    return bytes(out)


# ---- FAT build -------------------------------------------------------------
def geometry(tot, S):
    spc, rsvd, nf, rootents, fatsz, bits = 1, 1, 2, 512, 1, 12
    while True:
        for _ in range(3):
            rs = (rootents * 32 + S - 1) // S
            nclus = (tot - rsvd - nf * fatsz - rs) // spc
            fatsz = ((nclus + 2) * bits // 8 + S - 1) // S
        if bits == 12 and nclus >= 4085:
            bits = 16
            continue
        if bits == 16 and nclus >= 65525:
            sys.exit('volume needs FAT32: not supported by mkfat (use 512 B sectors or a smaller size)')  # ponytail
        return spc, rsvd, nf, rootents, fatsz, bits, nclus


def dos_time(t):
    lt = time.localtime(t)
    return ((max(lt.tm_year, 1980) - 1980) << 9 | lt.tm_mon << 5 | lt.tm_mday,
            lt.tm_hour << 11 | lt.tm_min << 5 | lt.tm_sec // 2)


SFN_OK = set(b'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&\'()-@^_`{}~')


def sfn_sum(n):
    s = 0
    for c in n:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def short_name(name, taken):
    """8.3 name; needs_lfn when the long name can't be represented exactly"""
    up = name.upper()
    base, _, ext = up.rpartition('.') if '.' in up[1:] else (up, '', '')
    clean = lambda s: bytes(c if c in SFN_OK else ord('_') for c in s.replace(' ', '').replace('.', '').encode('ascii', 'replace'))
    b, e = clean(base), clean(ext)[:3]
    exact = name == up and len(base) <= 8 and len(ext) <= 3 and b == base.encode() and e == ext.encode()
    if exact:
        sfn = b.ljust(8) + e.ljust(3)
        if sfn not in taken:
            return sfn, False
    for n in range(1, 1000000):
        tail = b'~%d' % n
        sfn = (b[:8 - len(tail)] + tail).ljust(8) + e.ljust(3)
        if sfn not in taken:
            return sfn, True
    sys.exit('too many similar names')


class Builder:
    def __init__(self, tot, S):
        self.S = S
        self.spc, self.rsvd, self.nf, self.rootents, self.fatsz, self.bits, self.nclus = geometry(tot, S)
        self.tot = tot
        self.img = bytearray(tot * S)
        self.fat = [0] * (self.nclus + 2)
        self.fat[0], self.fat[1] = (0xFF8, 0xFFF) if self.bits == 12 else (0xFFF8, 0xFFFF)
        self.next = 2
        self.root_sec = self.rsvd + self.nf * self.fatsz
        self.data_sec = self.root_sec + (self.rootents * 32 + S - 1) // S
        self.csize = self.spc * S

    def alloc(self, nbytes):
        n = max(1, -(-nbytes // self.csize))
        if self.next + n > self.nclus + 2:
            sys.exit('image full')
        first = self.next
        for c in range(first, first + n):
            self.fat[c] = c + 1
        self.fat[first + n - 1] = 0xFFF if self.bits == 12 else 0xFFFF
        self.next += n
        return first

    def put(self, clus, data):
        off = (self.data_sec + (clus - 2) * self.spc) * self.S
        self.img[off:off + len(data)] = data

    def entries(self, items):
        """directory entries (LFN + SFN) for [(name, attr, clus, size, mtime)]"""
        out, taken = b'', set()
        for name, attr, clus, size, mt in items:
            sfn, lfn = short_name(name, taken)
            taken.add(sfn)
            if lfn:
                u = name.encode('utf-16-le') + b'\0\0'
                u += b'\xff' * (-len(u) % 26)
                parts = [u[i:i + 26] for i in range(0, len(u), 26)]
                cs = sfn_sum(sfn)
                for k in range(len(parts), 0, -1):
                    p = parts[k - 1]
                    out += bytes([k | (0x40 if k == len(parts) else 0)]) + p[:10] + bytes([0x0F, 0, cs]) + \
                        p[10:22] + b'\0\0' + p[22:26]
            d, t = dos_time(mt)
            out += sfn + struct.pack('<BBBHHHHHHHI', attr, 0, 0, t, d, d, clus >> 16, t, d, clus & 0xFFFF, size)
        return out

    def add_dir(self, path, clus=None, parent=0):
        items = []
        for name in sorted(os.listdir(path)):
            full = os.path.join(path, name)
            st = os.stat(full)
            if os.path.isdir(full):
                items.append((name, 0x10, None, 0, st.st_mtime, full))
            else:
                data = open(full, 'rb').read()
                c = self.alloc(len(data)) if data else 0
                if data:
                    self.put(c, data)
                items.append((name, 0x20, c, len(data), st.st_mtime, None))
        # subdirectories: allocate after files so sizes are known
        def dot(n, c):
            d, t = dos_time(time.time())
            return n.ljust(11).encode() + struct.pack('<BBBHHHHHHHI', 0x10, 0, 0, t, d, d, c >> 16, t, d, c & 0xFFFF, 0)
        sub = []
        for i, it in enumerate(items):
            if it[1] == 0x10:
                n_ent = 2 + len(os.listdir(it[5])) * 21               # worst case LFN length
                c = self.alloc(n_ent * 32)
                items[i] = (it[0], 0x10, c, 0, it[4], it[5])
                sub.append((it[5], c))
        body = self.entries([it[:5] for it in items])
        if clus is None:
            if len(body) > self.rootents * 32:
                sys.exit('root directory full')
            off = self.root_sec * self.S
            self.img[off:off + len(body)] = body
        else:
            self.put(clus, dot('.', clus) + dot('..', parent) + body)
        for p, c in sub:
            self.add_dir(p, c, clus or 0)

    def finish(self):
        S, b = self.S, bytearray(self.S)
        b[0:3] = b'\xeb\x3c\x90'
        b[3:11] = b'MSDOS5.0'
        struct.pack_into('<HBHBH', b, 11, S, self.spc, self.rsvd, self.nf, self.rootents)
        if self.tot < 65536:
            struct.pack_into('<H', b, 19, self.tot)
        else:
            struct.pack_into('<I', b, 32, self.tot)
        b[21] = 0xF8
        struct.pack_into('<HHH', b, 22, self.fatsz, 63, 255)
        b[36], b[38] = 0x80, 0x29
        struct.pack_into('<I', b, 39, int.from_bytes(os.urandom(4), 'little'))
        b[43:54] = b'NO NAME    '
        b[54:62] = b'FAT12   ' if self.bits == 12 else b'FAT16   '
        b[510:512] = b'\x55\xaa'
        self.img[0:S] = b
        f = bytearray(self.fatsz * S)
        if self.bits == 12:
            for c, v in enumerate(self.fat):
                o = c * 3 // 2
                if c & 1:
                    f[o] = (f[o] & 0x0F) | (v << 4 & 0xF0); f[o + 1] = v >> 4 & 0xFF
                else:
                    f[o] = v & 0xFF; f[o + 1] = (f[o + 1] & 0xF0) | (v >> 8 & 0x0F)
        else:
            for c, v in enumerate(self.fat):
                struct.pack_into('<H', f, 2 * c, v)
        for k in range(self.nf):
            o = (self.rsvd + k * self.fatsz) * S
            self.img[o:o + len(f)] = f
        return bytes(self.img)


def mkfat(src, size, wl, S):
    if size % S:
        sys.exit('size must be a multiple of the sector size')
    tot = (wl_geometry(size, S)[3] if wl else size) // S
    b = Builder(tot, S)
    b.add_dir(src)
    img = b.finish()
    return wl_wrap(img, size, S) if wl else img


# ---- FAT parse -------------------------------------------------------------
def parse(img, extract=None, out=print):
    S, spc, rsvd, nf, rootents, tot16 = struct.unpack_from('<HBHBH', img, 11) + struct.unpack_from('<H', img, 19)
    fatsz = struct.unpack_from('<H', img, 22)[0] or struct.unpack_from('<I', img, 36)[0]
    tot = tot16 or struct.unpack_from('<I', img, 32)[0]
    rs = (rootents * 32 + S - 1) // S
    data = rsvd + nf * fatsz + rs
    nclus = (tot - data) // spc
    bits = 12 if nclus < 4085 else 16 if nclus < 65525 else 32
    fat = img[rsvd * S:(rsvd + fatsz) * S]
    eoc = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[bits]

    def nxt(c):
        if bits == 12:
            v = fat[c * 3 // 2] | fat[c * 3 // 2 + 1] << 8
            return v >> 4 if c & 1 else v & 0xFFF
        return struct.unpack_from('<H', fat, 2 * c)[0] if bits == 16 else struct.unpack_from('<I', fat, 4 * c)[0] & 0x0FFFFFFF

    def chain(c):
        out_ = b''
        seen = 0
        while 2 <= c < eoc and seen <= nclus:
            o = (data + (c - 2) * spc) * S
            out_ += img[o:o + spc * S]
            c = nxt(c); seen += 1
        return out_

    def entries(raw):
        lfn = {}
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = {}; continue
            if e[11] == 0x0F:
                lfn[e[0] & 0x1F] = e[1:11] + e[14:26] + e[28:32]
                continue
            if e[11] & 0x08:
                lfn = {}; continue
            if lfn:
                name = b''.join(lfn[k] for k in sorted(lfn)).decode('utf-16-le').split('\0')[0]
            else:
                b, x = e[0:8].decode('ascii', 'replace').rstrip(), e[8:11].decode('ascii', 'replace').rstrip()
                if e[12] & 0x08: b = b.lower()
                if e[12] & 0x10: x = x.lower()
                name = b + ('.' + x if x else '')
            lfn = {}
            if name in ('.', '..'):
                continue
            c = struct.unpack_from('<H', e, 26)[0] | (struct.unpack_from('<H', e, 20)[0] << 16 if bits == 32 else 0)
            yield name, bool(e[11] & 0x10), c, struct.unpack_from('<I', e, 28)[0]

    def walk(raw, path, depth):
        for name, isdir, c, size in entries(raw):
            p = os.path.join(path, name)
            out('%s%s%s  %d' % ('  ' * depth, name, '/' if isdir else '', size))
            if isdir:
                if extract:
                    os.makedirs(os.path.join(extract, p), exist_ok=True)
                walk(chain(c), p, depth + 1)
            elif extract:
                open(os.path.join(extract, p), 'wb').write(chain(c)[:size])

    out('FAT%d, %d B sectors, %d clusters of %d B' % (bits, S, nclus, spc * S))
    if extract:
        os.makedirs(extract, exist_ok=True)
    root = chain(struct.unpack_from('<I', img, 44)[0]) if bits == 32 else img[(rsvd + nf * fatsz) * S:data * S]
    walk(root, '', 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    m = sp.add_parser('mkfat'); m.add_argument('dir'); m.add_argument('out'); m.add_argument('size')
    m.add_argument('--wl', action='store_true'); m.add_argument('--sector', type=int, default=S_DEFAULT)
    p = sp.add_parser('parse'); p.add_argument('image'); p.add_argument('--wl', action='store_true')
    p.add_argument('-x', dest='extract')
    a = ap.parse_args()
    if a.cmd == 'mkfat':
        open(a.out, 'wb').write(mkfat(a.dir, int(a.size, 0), a.wl, a.sector))
    else:
        img = open(a.image, 'rb').read()
        parse(wl_unwrap(img) if a.wl else img, a.extract)


if __name__ == '__main__':
    main()
