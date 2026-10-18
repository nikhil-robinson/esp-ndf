#!/usr/bin/env python3
"""NVS partition tool: generate a partition image from CSV, parse an image
(or a flash dump), create encryption keys. Output is byte-identical to
ESP-IDF's nvs_partition_gen.py (format v2, optional XTS-AES encryption).

  nvstool.py gen    input.csv out.bin SIZE [--keyfile keys.bin]
  nvstool.py parse  image.bin [--keyfile keys.bin]
  nvstool.py keygen keys.bin [--hmac-key hmac.bin]

CSV: key,type,encoding,value  (type: namespace | data | file)
encodings: u8 i8 u16 i16 u32 i32 u64 i64 string hex2bin base64 binary
"""
import argparse, base64, csv, os, struct, sys, zlib

PAGE, ENTRY, FIRST, MAX_ENT = 4096, 32, 64, 126
ACTIVE, FULL = 0xFFFFFFFE, 0xFFFFFFFC
VER2 = 0xFE
T_U8, T_I8, T_U16, T_I16, T_U32, T_I32, T_U64, T_I64 = 0x01, 0x11, 0x02, 0x12, 0x04, 0x14, 0x08, 0x18
T_SZ, T_BLOB, T_BLOB_DATA, T_BLOB_IDX = 0x21, 0x41, 0x42, 0x48
PRIM = {'u8': (T_U8, '<B'), 'i8': (T_I8, '<b'), 'u16': (T_U16, '<H'), 'i16': (T_I16, '<h'),
        'u32': (T_U32, '<I'), 'i32': (T_I32, '<i'), 'u64': (T_U64, '<Q'), 'i64': (T_I64, '<q')}
TYPE_NAME = {v[0]: k for k, v in PRIM.items()} | {T_SZ: 'string', T_BLOB: 'blob(v1)',
                                                 T_BLOB_DATA: 'blob_data', T_BLOB_IDX: 'blob_index'}
MAX_STR = 4000


def crc(b):
    return zlib.crc32(b, 0xFFFFFFFF)


# ---- AES (FIPS-197) + XTS, small and slow: enough for NVS images ----------
def _xt(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a, b = _xt(a), b >> 1
    return r


SBOX = [0] * 256
_p = _q = 1
for _ in range(255):                       # generate S-box from GF(2^8) inverses
    _p = _p ^ _xt(_p)
    _q ^= _q << 1; _q ^= _q << 2; _q ^= _q << 4; _q &= 0xFF
    if _q & 0x80:
        _q ^= 0x09
    x = _q ^ ((_q << 1 | _q >> 7) & 0xFF) ^ ((_q << 2 | _q >> 6) & 0xFF) ^ \
        ((_q << 3 | _q >> 5) & 0xFF) ^ ((_q << 4 | _q >> 4) & 0xFF)
    SBOX[_p] = x ^ 0x63
SBOX[0] = 0x63
INV = [0] * 256
for i, v in enumerate(SBOX):
    INV[v] = i


def _expand(key):
    nk = len(key) // 4
    nr = nk + 6
    w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
    rc = 1
    for i in range(nk, 4 * (nr + 1)):
        t = list(w[i - 1])
        if i % nk == 0:
            t = [SBOX[b] for b in t[1:] + t[:1]]
            t[0] ^= rc
            rc = _xt(rc)
        elif nk > 6 and i % nk == 4:
            t = [SBOX[b] for b in t]
        w.append([a ^ b for a, b in zip(w[i - nk], t)])
    return [sum(w[4 * r:4 * r + 4], []) for r in range(nr + 1)]


def _enc(rk, b):
    s = [x ^ k for x, k in zip(b, rk[0])]
    for r in range(1, len(rk)):
        s = [SBOX[x] for x in s]
        s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]           # shift rows
        if r != len(rk) - 1:
            n = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                n += [_xt(a[0]) ^ _xt(a[1]) ^ a[1] ^ a[2] ^ a[3], a[0] ^ _xt(a[1]) ^ _xt(a[2]) ^ a[2] ^ a[3],
                      a[0] ^ a[1] ^ _xt(a[2]) ^ _xt(a[3]) ^ a[3], _xt(a[0]) ^ a[0] ^ a[1] ^ a[2] ^ _xt(a[3])]
            s = n
        s = [x ^ k for x, k in zip(s, rk[r])]
    return bytes(s)


def _dec(rk, b):
    s = [x ^ k for x, k in zip(b, rk[-1])]
    for r in range(len(rk) - 2, -1, -1):
        s = [s[(i - 4 * (i % 4)) % 16] for i in range(16)]           # inverse shift rows
        s = [INV[x] for x in s]
        s = [x ^ k for x, k in zip(s, rk[r])]
        if r:
            n = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                n += [_mul(a[0], 14) ^ _mul(a[1], 11) ^ _mul(a[2], 13) ^ _mul(a[3], 9),
                      _mul(a[0], 9) ^ _mul(a[1], 14) ^ _mul(a[2], 11) ^ _mul(a[3], 13),
                      _mul(a[0], 13) ^ _mul(a[1], 9) ^ _mul(a[2], 14) ^ _mul(a[3], 11),
                      _mul(a[0], 11) ^ _mul(a[1], 13) ^ _mul(a[2], 9) ^ _mul(a[3], 14)]
            s = n
    return bytes(s)


def xts(key64, addr, data, decrypt=False):
    """XTS-AES-256 over one 32-byte NVS entry; tweak = entry address (LE)."""
    k1, k2 = _expand(key64[:32]), _expand(key64[32:])
    t = int.from_bytes(_enc(k2, addr.to_bytes(16, 'little')), 'little')
    out = b''
    for j in range(0, len(data), 16):
        tb = t.to_bytes(16, 'little')
        x = bytes(a ^ b for a, b in zip(data[j:j + 16], tb))
        x = _dec(k1, x) if decrypt else _enc(k1, x)
        out += bytes(a ^ b for a, b in zip(x, tb))
        t = ((t << 1) ^ (0x87 if t >> 127 else 0)) & ((1 << 128) - 1)
    return out


# ---- generator ----------------------------------------------------------
class Gen:
    def __init__(self, size, key=None):
        if size % PAGE or size < PAGE:
            sys.exit('size must be a multiple of 4096')
        self.read_only = size - PAGE < 2 * PAGE
        self.budget = size if self.read_only else size - PAGE   # one page reserved
        self.total = size
        self.key = key
        self.pages = []
        self.ns = {}
        self.ns_count = 0
        self.new_page()

    def new_page(self):
        if self.pages:
            p = self.pages[-1]
            if struct.unpack_from('<I', p, 0)[0] == ACTIVE:
                struct.pack_into('<I', p, 0, FULL)
        if len(self.pages) * PAGE >= self.budget:
            sys.exit('Error: size is less than the size of data in csv')
        p = bytearray(b'\xff' * PAGE)
        struct.pack_into('<II', p, 0, ACTIVE, len(self.pages))
        p[8] = VER2
        struct.pack_into('<I', p, 28, crc(bytes(p[4:28])))
        self.pages.append(p)
        self.n = 0                                   # entries used in the current page

    def put(self, data, count):
        """write `count` entries (data padded with 0xFF) at the current slot"""
        p = self.pages[-1]
        data = bytes(data) + b'\xff' * (count * ENTRY - len(data))
        page_no = len(self.pages) - 1
        for i in range(count):
            e = data[i * ENTRY:(i + 1) * ENTRY]
            off = FIRST + (self.n + i) * ENTRY
            if self.key:
                e = xts(self.key, page_no * PAGE + off, e)
            p[off:off + ENTRY] = e
            bit = (self.n + i) * 2                   # state 0b11 empty -> 0b10 written
            p[32 + bit // 8] &= ~(1 << (bit & 7)) & 0xFF
        self.n += count

    @staticmethod
    def head(ns, typ, span, chunk, key):
        e = bytearray(b'\xff' * ENTRY)
        e[0:4] = bytes([ns, typ, span, chunk])
        e[8:24] = key.encode().ljust(16, b'\0')
        return e

    def seal(self, e):
        struct.pack_into('<I', e, 4, crc(bytes(e[0:4] + e[8:32])))
        return e

    def primitive(self, ns, key, enc, val):
        if self.n >= MAX_ENT:
            self.new_page()
        typ, fmt = PRIM[enc]
        e = self.head(ns, typ, 1, 0xFF, key)
        struct.pack_into(fmt, e, 24, val)
        self.put(self.seal(e), 1)

    def string(self, ns, key, data):
        if len(data) > MAX_STR:
            sys.exit('%s: string longer than %d bytes' % (key, MAX_STR))
        cnt = (len(data) + 31) // 32
        if self.n + cnt + 1 >= MAX_ENT:
            self.new_page()
        e = self.head(ns, T_SZ, cnt + 1, 0xFF, key)
        struct.pack_into('<H', e, 24, len(data))
        struct.pack_into('<I', e, 28, crc(data))
        self.put(self.seal(e), 1)
        self.put(data, cnt)

    def blob(self, ns, key, data):
        if self.n >= MAX_ENT:
            self.new_page()
        off, chunks, left = 0, 0, len(data)
        while True:
            room = (MAX_ENT - self.n - 1) * ENTRY
            size = min(room, left)
            left -= size
            cnt = (size + 31) // 32
            chunk = data[off:off + size]
            e = self.head(ns, T_BLOB_DATA, cnt + 1, chunks, key)
            struct.pack_into('<H', e, 24, size)
            struct.pack_into('<I', e, 28, crc(chunk))
            self.put(self.seal(e), 1)
            self.put(chunk, cnt)
            chunks += 1
            if left or room - size < ENTRY:
                self.new_page()
            off += size
            if not left:
                e = self.head(ns, T_BLOB_IDX, 1, 0xFF, key)
                struct.pack_into('<IBB', e, 24, len(data), chunks, 0)
                self.put(self.seal(e), 1)
                return

    def namespace(self, name):
        if name in self.ns:
            return self.ns[name]
        self.ns_count += 1
        self.ns[name] = self.ns_count
        self.primitive(0, name, 'u8', self.ns_count)
        return self.ns_count

    def image(self):
        out = b''.join(self.pages)
        return out + b'\xff' * (self.total - len(out))


def value_bytes(row, base):
    t, enc, v = row['type'], row['encoding'].lower(), row['value']
    if t == 'file':
        path = v if os.path.isabs(v) else os.path.join(base, v)
        raw = open(path, 'rb').read()
        if enc == 'string':
            return raw.decode() + '\0'
        if enc == 'hex2bin':
            return bytes.fromhex(raw.decode().strip())
        if enc == 'base64':
            return base64.b64decode(raw)
        return raw
    if enc == 'string':
        return v + '\0'
    if enc == 'hex2bin':
        return bytes.fromhex(v.strip())
    if enc == 'base64':
        return base64.b64decode(v)
    if enc == 'binary':
        return v.encode()
    return int(v, 0)


def generate(csv_path, size, key=None):
    g = Gen(size, key)
    ns = None
    with open(csv_path, encoding='utf8') as f:
        for row in csv.DictReader((l for l in f if not l.startswith('#')), delimiter=','):
            k = row['key']
            if len(k) > 15:
                sys.exit('key %s longer than 15 characters' % k)
            if row['type'] == 'namespace':
                ns = g.namespace(k)
                continue
            if ns is None:
                sys.exit('entry %s before any namespace' % k)
            enc = row['encoding'].lower()
            v = value_bytes(row, os.getcwd())
            if enc in PRIM:
                g.primitive(ns, k, enc, v)
            elif enc == 'string':
                g.string(ns, k, v.encode())
            elif enc in ('hex2bin', 'base64', 'binary'):
                g.blob(ns, k, v)
            else:
                sys.exit('%s: unsupported encoding %s' % (k, enc))
    return g.image()


# ---- parser -------------------------------------------------------------
STATE = {0xFFFFFFFF: 'EMPTY', ACTIVE: 'ACTIVE', FULL: 'FULL', 0xFFFFFFF8: 'FREEING', 0: 'CORRUPT'}


def parse(img, key=None, out=print):
    """Print every page and entry, check CRCs; returns {(ns, key): value}."""
    names, items, blobs, errors = {0: ''}, {}, {}, 0
    for pn in range(len(img) // PAGE):
        p = img[pn * PAGE:(pn + 1) * PAGE]
        state, seq, ver = struct.unpack_from('<IIB', p, 0)
        if state == 0xFFFFFFFF:
            continue
        hok = struct.unpack_from('<I', p, 28)[0] == crc(p[4:28])
        out('page %d: %s seq %d version %d %s' % (pn, STATE.get(state, hex(state)), seq,
                                                    2 if ver == VER2 else 1, '' if hok else 'HEADER CRC BAD'))
        errors += not hok
        i = 0
        while i < MAX_ENT:
            bits = (p[32 + i * 2 // 8] >> (i * 2 % 8)) & 3
            if bits != 2:                            # 3 empty, 0 erased
                i += 1
                continue
            raw = lambda j: (xts(key, pn * PAGE + FIRST + j * ENTRY, p[FIRST + j * ENTRY:FIRST + (j + 1) * ENTRY], True)
                             if key else p[FIRST + j * ENTRY:FIRST + (j + 1) * ENTRY])
            e = raw(i)
            ns, typ, span, chunk = e[0], e[1], e[2], e[3]
            k = e[8:24].split(b'\0')[0].decode(errors='replace')
            ok = struct.unpack_from('<I', e, 4)[0] == crc(e[0:4] + e[8:32])
            span = max(span, 1)
            desc = ''
            if typ in TYPE_NAME and typ in (T_SZ, T_BLOB, T_BLOB_DATA):
                size, dcrc = struct.unpack_from('<HxxI', e, 24)
                data = b''.join(raw(i + 1 + j) for j in range(span - 1))[:size]
                dok = crc(data) == dcrc
                ok &= dok
                if typ == T_SZ:
                    val = data[:-1].decode(errors='replace')
                    items[(names.get(ns, ns), k)] = val
                    desc = '%r' % val
                else:
                    blobs.setdefault((ns, k), {})[chunk] = data
                    desc = '%d B chunk %d' % (size, chunk)
            elif typ == T_BLOB_IDX:
                size, cnt, start = struct.unpack_from('<IBB', e, 24)
                parts = blobs.get((ns, k), {})
                data = b''.join(parts.get(start + c, b'') for c in range(cnt))
                ok &= len(data) == size
                items[(names.get(ns, ns), k)] = data
                desc = '%d B in %d chunks' % (size, cnt)
            elif typ in TYPE_NAME:
                fmt = PRIM[TYPE_NAME[typ]][1]
                val = struct.unpack_from(fmt, e, 24)[0]
                if ns == 0:
                    names[val] = k
                    desc = 'namespace -> %d' % val
                else:
                    items[(names.get(ns, ns), k)] = val
                    desc = str(val)
            else:
                desc = 'unknown type 0x%02x' % typ
                ok = False
            errors += not ok
            out('  [%3d] ns %-3d %-10s %-15s %s%s' % (i, ns, TYPE_NAME.get(typ, '?'), k, desc, '' if ok else '  CRC BAD'))
            i += span
    out('%d errors' % errors)
    return items


def read_keys(path):
    b = open(path, 'rb').read()
    if struct.unpack_from('<I', b, 64)[0] != crc(b[:64]):
        sys.exit('keys file CRC mismatch')
    return b[:64]


def keygen(path, hmac_key=None):
    if hmac_key:                                     # same derivation as the device HMAC scheme
        import hmac, hashlib
        k = hmac.new(hmac_key, b'\x5a\x5a\xbe\xae' * 8, hashlib.sha256).digest() + \
            hmac.new(hmac_key, b'\xa5\xa5\xde\xce' * 8, hashlib.sha256).digest()
    else:
        k = os.urandom(64)
    buf = bytearray(b'\xff' * PAGE)
    buf[:64] = k
    struct.pack_into('<I', buf, 64, crc(k))
    open(path, 'wb').write(buf)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    g = sp.add_parser('gen'); g.add_argument('csv'); g.add_argument('out'); g.add_argument('size')
    g.add_argument('--keyfile')
    p = sp.add_parser('parse'); p.add_argument('image'); p.add_argument('--keyfile')
    k = sp.add_parser('keygen'); k.add_argument('out'); k.add_argument('--hmac-key')
    a = ap.parse_args()
    if a.cmd == 'gen':
        key = read_keys(a.keyfile) if a.keyfile else None
        open(a.out, 'wb').write(generate(a.csv, int(a.size, 0), key))
    elif a.cmd == 'parse':
        parse(open(a.image, 'rb').read(), read_keys(a.keyfile) if a.keyfile else None)
    else:
        keygen(a.out, open(a.hmac_key, 'rb').read() if a.hmac_key else None)


if __name__ == '__main__':
    main()
