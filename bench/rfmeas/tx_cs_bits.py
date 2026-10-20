#!/usr/bin/env python3
"""Which send-CS bits change the on-air format? Flip one bit (D0), receive with defaults (C9:48): a whitening or CRC
switch shows as wrong bytes / CRC errors / nothing. Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time
cases = [(0x02, b) for b in range(16)] + [(0x04, b) for b in range(4, 16)] + [(0x18, b) for b in range(8, 16)] + [(0x14, b) for b in range(16)] + [(0x16, b) for b in range(6, 16)]
for off, b in cases:
    tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); res = {}
    tx.cmd('txflip %#x %#x' % (off, 1 << b))
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rx 2440 0 0x71764129 0 127 1500', timeout=30)[0])); th.start(); time.sleep(0.3)
    try: t = tx.cmd('send 2440 0 0x71764129 0 37 1000', timeout=20)[0]
    except Exception: t = 'tx hang'
    th.join(timeout=30); r = res.get('rx', '?')
    flag = '' if (' good 9' in r or ' good 1000' in r) and ' wrong 0 ' in r and 'crcbad 0 ' in r else '   <== changed'
    print('CS+%#04x bit %2d: %s | %s%s' % (off, b, t, r, flag), flush=True)
