#!/usr/bin/env python3
"""Continuation of tx_cs_bits.py: send-CS word +0x00 bits 5..15 and RWBLECNTL bits 10..15 on the transmitter.
Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time
cases = [('cs', 0x00, b) for b in range(5, 16)] + [('core', 0, b) for b in range(10, 16)]
for kind, off, b in cases:
    tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); res = {}
    if kind == 'cs': tx.cmd('txflip %#x %#x' % (off, 1 << b))
    else: tx.cmd('send 2440 0 0x71764129 0 37 1', timeout=10); tx.cmd('core %#x 0' % (1 << b))
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rx 2440 0 0x71764129 0 127 1500', timeout=30)[0])); th.start(); time.sleep(0.3)
    try: t = tx.cmd('send 2440 0 0x71764129 0 37 1000', timeout=20)[0]
    except Exception: t = 'tx hang'
    th.join(timeout=30); r = res.get('rx', '?')
    flag = '' if (' good 9' in r or ' good 1000' in r) and ' wrong 0 ' in r and 'crcbad 0 ' in r else '   <== changed'
    print('%s %#04x bit %2d: %s | %s%s' % (kind, off, b, t, r, flag), flush=True)
