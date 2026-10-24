#!/usr/bin/env python3
"""Read and decode the esp-ndf core dump (soc/esp32s3/coredump.c) from the "coredump" partition.
Usage: coredump.py PORT build/<app>/app.elf [--offset 0xA0000]"""
import struct, subprocess, sys, tempfile, glob, os
port, elf = sys.argv[1], sys.argv[2]
off = int(sys.argv[sys.argv.index('--offset') + 1], 0) if '--offset' in sys.argv else 0xA0000
tmp = tempfile.mktemp()
subprocess.run(['esptool.py', '--chip', 'esp32s3', '-p', port, 'read_flash', hex(off), '0x3000', tmp], check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
d = open(tmp, 'rb').read(); os.remove(tmp)
fmt = '<5I64s20II8II'
hdr = struct.unpack_from(fmt, d)
magic, ver, core, pc, sp, why = hdr[:6]
if magic != 0x4443444E:
    sys.exit('no core dump (magic %08x)' % magic)
frame, n_bt, bt, slen = hdr[6:26], hdr[26], hdr[27:35], hdr[35]
stack = d[struct.calcsize(fmt):struct.calcsize(fmt) + slen]
a2l = glob.glob(os.path.expanduser('~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line'))[0]
def where(a):
    return subprocess.run([a2l, '-pfe', elf, hex(a)], capture_output=True, text=True).stdout.strip().replace(os.getcwd() + '/', '')
print('core dump v%d: core %d, %s' % (ver, core, why.split(b'\0')[0].decode()))
print('  PC 0x%08x  %s' % (pc, where(pc)))
print('  SP 0x%08x, %d stack bytes saved' % (sp, slen))
names = ['a0', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7', 'a8', 'a9', 'a10', 'a11', 'SAR', 'LBEG', 'LEND', 'LCOUNT', 'CPEN', 'a12', 'a13', 'a14', 'a15']
print('  ' + ' '.join('%s=%08x' % (n, v) for n, v in zip(names, frame)))
print('  backtrace:')
for a in bt[:n_bt]:
    print('    0x%08x  %s' % (a, where(a)))
print('=== coredump ok ===')
