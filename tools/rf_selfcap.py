#!/usr/bin/env python3
"""Receive while transmitting, one board (examples/rf/rf_selfcap): every TX mode x a few offsets / gains. usage: rf_selfcap.py [port]
Data: bench/rfmeas/data/selfcap.txt"""
import sys, rfhost
b = rfhost.Board(sys.argv[1] if len(sys.argv) > 1 else rfhost.D0)
print(b.cmd('open', timeout=30))
for mode in (0, 1, 2, 3):
    for off in (1000, 2500):
        for g in (0, 10, 30):
            print([l for l in b.cmd('selfcap %d %d %d' % (mode, off, g), timeout=30) if l.startswith('selfcap')][0], flush=True)
