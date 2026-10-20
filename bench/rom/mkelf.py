#!/usr/bin/env python3
"""Symbolised ESP32-S3 ROM ELF for objdump: rom.bin (esptool dump_mem 0x40000000 0x60000, not committed) + the IDF
ROM linker scripts; ROM jump-table entries (l32r a9 / jx a9) also name their target as <name>_impl."""
import glob, os, re, struct, subprocess
B = os.path.expanduser('~/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20250707/xtensa-esp-elf/bin/xtensa-esp32s3-elf-')
LD = os.path.expanduser('~/idf5_3_3/.espressif/v5.3.5/esp-idf/components/esp_rom/esp32s3/ld/')
rom = open('rom.bin', 'rb').read(); base = 0x40000000
syms = {}
for f in glob.glob(LD + '*.ld'):
    for m in re.finditer(r'^\s*([A-Za-z_]\w*)\s*=\s*(0x40[0-5][0-9a-fA-F]{5})\s*;', open(f).read(), re.M):
        syms[m.group(1)] = int(m.group(2), 16)
for n, a in list(syms.items()):
    o = a - base
    b0, b1, b2 = rom[o], rom[o + 1], rom[o + 2]
    if b0 & 0xF == 1 and rom[o + 3:o + 6] == b'\xa0\x09\x00':        # l32r aX, lit ; jx a9
        imm = (b2 << 8 | b1) - 0x10000
        lit = ((a + 3) & ~3) + (imm << 2)
        if base <= lit < base + len(rom):
            t = struct.unpack_from('<I', rom, lit - base)[0]
            if base <= t < base + len(rom): syms.setdefault(n + '_impl', t)
with open('syms.args', 'w') as f:
    for n, a in sorted(syms.items()): f.write('--add-symbol %s=.text:%d,function,global\n' % (n, a - base))
subprocess.check_call([B + 'objcopy', '-I', 'binary', '-O', 'elf32-xtensa-le', '-B', 'xtensa',
                       '--rename-section', '.data=.text,code,alloc,load,readonly,contents', 'rom.bin', 'r0.elf'])
subprocess.check_call([B + 'objcopy', '--change-section-address', '.text=0x40000000', '@syms.args', 'r0.elf', 'rom.elf'])
os.remove('r0.elf'); print(len(syms), 'symbols')
