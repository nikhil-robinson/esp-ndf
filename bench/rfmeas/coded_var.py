#!/usr/bin/env python3
"""Coded (S8) receive: per-boot vs within-boot variation. N boots of C9:48, 3 rx runs each, D0 sending S8.
With 'dump': after the runs, a full register dump (rxdump 2) per boot -> data/coded_dump_<boot>.txt.
Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time, sys, os
N = int(sys.argv[1]) if len(sys.argv) > 1 else 6; dump = 'dump' in sys.argv; rate = 2
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data'); os.makedirs(out, exist_ok=True)
def run(tx, rx):
    res = {}
    th = threading.Thread(target=lambda: res.setdefault('r', [l for l in rx.cmd('rx 2404 %d 0x71764129 0 -1 1500' % rate, timeout=40) if l.startswith('rx rc')][0])); th.start(); time.sleep(0.3)
    tx.cmd('send 2404 %d 0x71764129 0 100 150' % rate, timeout=20); th.join(timeout=40)
    return int(res.get('r', 'rx rc 0 good -1').split()[4])
tx = rfhost.Board(rfhost.D0)
for b in range(N):
    rx = rfhost.Board(rfhost.C948)
    g = [run(tx, rx) for _ in range(3)]
    line = 'boot %d good %s (of 150)' % (b, g)
    if dump:
        d = rx.cmd('rxdump %d' % rate, timeout=60)
        open(os.path.join(out, 'coded_dump_%d.txt' % b), 'w').write('# good %s\n' % g + '\n'.join(d) + '\n')
        d2 = rx.cmd('rxdump %d' % rate, timeout=60)
        open(os.path.join(out, 'coded_dump_%d_b.txt' % b), 'w').write('\n'.join(d2) + '\n')
    print(line, flush=True); rx.s.close()
