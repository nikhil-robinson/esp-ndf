#!/usr/bin/env python3
"""Partition table CSV -> binary (ESP-IDF compatible format, MD5 entry).
usage: mkpart.py table.csv out.bin
CSV columns: name, type, subtype, offset, size[, flags]  (# comments allowed)"""
import hashlib, struct, sys

TYPES = {'app': 0x00, 'data': 0x01}
SUBTYPES = {
    0x00: {'factory': 0x00, 'test': 0x20, **{f'ota_{i}': 0x10 + i for i in range(16)}},
    0x01: {'ota': 0x00, 'phy': 0x01, 'nvs': 0x02, 'coredump': 0x03, 'nvs_keys': 0x04, 'efuse': 0x05,
           'undefined': 0x06, 'esphttpd': 0x80, 'fat': 0x81, 'spiffs': 0x82, 'littlefs': 0x83},
}
FLAGS = {'encrypted': 1 << 0, 'readonly': 1 << 1}

def num(s):
    s = s.strip().upper()
    mult = 1
    if s.endswith('K'): mult, s = 1024, s[:-1]
    elif s.endswith('M'): mult, s = 1024 * 1024, s[:-1]
    return int(s, 0) * mult

def main(src, dst):
    out = b''
    for line in open(src):
        line = line.split('#')[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(',')] + ['']
        name, typ, sub, off, size, flags = f[:6]
        t = TYPES[typ] if typ in TYPES else num(typ)
        st = SUBTYPES.get(t, {}).get(sub, None)
        st = st if st is not None else num(sub)
        fl = 0
        for x in flags.split(':'):
            if x: fl |= FLAGS[x]
        if num(off) % 0x1000 or (t == 0 and num(off) % 0x10000):
            sys.exit(f'{name}: misaligned offset')
        out += struct.pack('<2sBBLL16sL', b'\xaa\x50', t, st, num(off), num(size), name.encode()[:16], fl)
    out += b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(out).digest()
    out += b'\xff' * (0xC00 - len(out))                     # pad to the table size IDF uses
    open(dst, 'wb').write(out)

if __name__ == '__main__':
    main(*sys.argv[1:3])
