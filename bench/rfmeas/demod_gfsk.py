#!/usr/bin/env python3
"""Packet-engine demodulator vs GFSK from D0's DAC (gfsk mhz rate dev len amp gap): which bit rates / deviations the
hardware receive path decodes (CRC on, data checked). Args: list of 'rate_kbps:dev_khz[:rx_rate]' (rx_rate = engine
rate code, default 0 = 1M). Optional rx-side pre-commands via env PRE (';'-separated). Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time, sys, os
cases = sys.argv[1:] or ['1000:250']
pre = [c for c in os.environ.get('PRE', '').split(';') if c]
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948)
for c in pre: rx.cmd(c)
for case in cases:
    p = case.split(':'); rate, dev = p[0], p[1]; rr = int(p[2]) if len(p) > 2 else 0
    g = tx.cmd('gfsk 2440 %s %s 20 200 100' % (rate, dev), timeout=30)[0]
    r = [x for x in rx.cmd('rx 2440 %d 0x71764129 0 -1 500' % rr, timeout=20) if x.startswith('rx rc')][0]
    print('%5s kbit/s dev %4s kHz rx rate %d | %s | %s' % (rate, dev, rr, g, r), flush=True)
tx.cmd('stop')
