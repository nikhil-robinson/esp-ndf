#!/usr/bin/env python3
"""Whitening-off search: set one RWBLECNTL bit on BOTH boards, CRC off on both (flags 1), payload 0..36.
A whitening switch round-trips exact data with the bit on both ends (and breaks with it on one end only).
Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time, sys
bits = [int(b) for b in sys.argv[1:]] or [b for b in range(10, 32) if b not in (13, 19)]
for b in [None] + bits:
    tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); res = {}
    tx.cmd('send 2440 0 0x71764129 1 37 1', timeout=10)          # engine up once on both boards: ble_hw_init
    rx.cmd('rx 2440 0 0x71764129 1 127 20', timeout=10)          # rewrites RWBLECNTL, so it must run before 'core'
    if b is not None:
        tx.cmd('core %#x 0' % (1 << b)); rx.cmd('core %#x 0' % (1 << b))
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rx 2440 0 0x71764129 1 127 1500', timeout=30)))
    th.start(); time.sleep(0.3)
    try: t = tx.cmd('send 2440 0 0x71764129 1 37 1000', timeout=20)[-1]
    except Exception as e: t = 'tx %s' % e
    th.join(timeout=30); r = res.get('rx', ['?'])
    print('bit %s: %s | %s | %s' % (b, t, r[0], r[-1]), flush=True)
    tx.s.close(); rx.s.close()
