#!/usr/bin/env python3
"""RWBLECNTL bit 14: on both / TX only / RX only, CRC off, payload 0..36; prints the first wrong packet's bytes."""
import rfhost, threading, time
for name, ontx, onrx in (('both', 1, 1), ('tx', 1, 0), ('rx', 0, 1)):
    tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); res = {}
    tx.cmd('send 2440 0 0x71764129 1 37 1', timeout=10); rx.cmd('rx 2440 0 0x71764129 1 127 20', timeout=10)
    if ontx: tx.cmd('core 0x4000 0')
    if onrx: rx.cmd('core 0x4000 0')
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rx 2440 0 0x71764129 1 127 1500', timeout=30))); th.start(); time.sleep(0.3)
    tx.cmd('send 2440 0 0x71764129 1 37 1000', timeout=20); th.join(timeout=30)
    print(name, *[l for l in res.get('rx', []) if 'bad' in l or 'good' in l], sep='\n  ', flush=True)
    tx.s.close(); rx.s.close()
