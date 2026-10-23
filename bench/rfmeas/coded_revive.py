#!/usr/bin/env python3
"""Receive dead state (all rates, no sync, until reboot): which action brings it back? Per action: fresh boot, run S8
windows until two are empty, apply the action, run 4 more. Run from tools/ with PYTHONPATH=."""
import rfhost, sys
tx = rfhost.Board(rfhost.D0); tx.cmd('send 2404 2 0x71764129 0 100 0', timeout=10)
for act in sys.argv[1:] or ['rxdc', 'reopen']:
    rx = rfhost.Board(rfhost.C948)
    win = lambda: int([x for x in rx.cmd('rx 2404 2 0x71764129 0 -1 300', timeout=8) if x.startswith('rx rc')][0].split()[4])
    seq = [win(), win()]
    while len(seq) < 40 and not (seq[-1] == 0 and seq[-2] == 0): seq.append(win())
    out = [o for a in act.split(';') for o in rx.cmd(a, timeout=30)]
    print('%-8s before %s | %s | after %s' % (act, seq, out, [win() for _ in range(4)]), flush=True); rx.s.close()
tx.cmd('stop')
