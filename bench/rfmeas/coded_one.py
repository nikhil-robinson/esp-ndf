#!/usr/bin/env python3
"""One coded link, full rx output. Args: rate [rx-side pre-commands separated by ';']. Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time, sys
import os; gain = os.environ.get('GAIN', '-1'); rate = int(sys.argv[1]); pre = [c for c in (sys.argv[2] if len(sys.argv) > 2 else '').split(';') if c]
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); res = {}
for c in pre: print(c, rx.cmd(c))
th = threading.Thread(target=lambda: res.setdefault('r', rx.cmd('rx 2404 %d 0x71764129 0 %s 1500' % (rate, gain), timeout=40))); th.start(); time.sleep(0.3)
print(tx.cmd('send 2404 %d 0x71764129 0 100 150' % rate, timeout=20)); th.join(timeout=40)
print(*res.get('r', ['none']), sep='\n')
