#!/usr/bin/env python3
"""Coded receive started in silence (first event = window timeout) never syncs afterwards. Fresh RX boot per case:
rx-side pre-commands, rx S8 1500 ms, TX (counted 150) starts 0.3 s later. Args: one case per argument, commands
separated by ';'. Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time, sys
tx = rfhost.Board(rfhost.D0)
for case in sys.argv[1:]:
    rx = rfhost.Board(rfhost.C948); res = {}
    for c in [c for c in case.split(';') if c]: rx.cmd(c)
    th = threading.Thread(target=lambda: res.setdefault('r', rx.cmd('rx 2404 2 0x71764129 0 -1 1700', timeout=40))); th.start(); time.sleep(0.3)
    tx.cmd('send 2404 2 0x71764129 0 100 150', timeout=20); th.join(timeout=40)
    r = res.get('r', [])
    print('%-60s %s | %s' % (case or 'default', [x for x in r if x.startswith('rx rc')][0].split()[4], [x for x in r if x.startswith('rx events')][0].split('nosync')[1]), flush=True)
    rx.s.close()
