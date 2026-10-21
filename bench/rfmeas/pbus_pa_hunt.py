#!/usr/bin/env python3
"""Which pbus-memory word holds the BT TX state's PA enable? C9:48 receives (rf_pkt rx, hardware AGC) with one word's
bits 7:4 cleared; if its replies stop, D0 (sending 37 B continuously) gets no acknowledgements: no empty packets
arrive and D0 keeps sending 1180/s. Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time
for idx in [None] + list(range(46)):
    tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); res = {}
    if idx is not None: rx.cmd('pm %d 0xFFFFFF0F' % idx)
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rx 2440 0 0x71764129 0 127 1500', timeout=30))); th.start(); time.sleep(0.3)
    t = tx.cmd('send 2440 0 0x71764129 0 37 0', timeout=10)[0].split(' cpu')[0]
    th.join(timeout=30); out = res.get('rx', ['?'] * 3)
    emp = [l for l in out if l.startswith('rx empty')]; r = [l for l in out if l.startswith('rx rc')]
    print('word %s: D0 %s | %s | %s' % (idx, t, emp[0] if emp else '?', r[0] if r else out), flush=True)
