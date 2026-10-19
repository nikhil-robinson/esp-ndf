#!/usr/bin/env python3
"""Name the rom_phyFuns table entries of a trace: offset address name.
  python3 name_table.py trace.txt rom.elf app.elf > table_named.txt"""
import subprocess, sys
trace, *elfs = sys.argv[1:]
names = {}
for i, elf in enumerate(elfs):
    out = subprocess.run(['xtensa-esp32s3-elf-nm', elf], capture_output=True, text=True).stdout
    for l in out.split('\n'):
        p = l.split()
        if len(p) == 3 and p[1] in 'TtW':
            names.setdefault(int(p[0], 16), ('app:' if i else '') + p[2])
for l in open(trace, errors='replace'):
    if l.startswith('@orig'):
        _, off, addr = l.split()
        a = int(addr, 16)
        print(off, addr, names.get(a, '?'))
