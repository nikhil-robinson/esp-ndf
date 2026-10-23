#!/usr/bin/env python3
"""Capture-and-diff for coded receive: full register dump (rxdump 2) on C9:48 with the forced front end (receives S8)
vs the hardware AGC (receives nothing), D0 sending S8 continuously. Two dumps per state; words that differ inside one
state are dropped as volatile. Output: data/coded_{forced,agc}_{0,1}.txt + the stable diff. Run from tools/ with PYTHONPATH=."""
import rfhost, os, re
d = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data'); os.makedirs(d, exist_ok=True)
tx = rfhost.Board(rfhost.D0); tx.cmd('send 2404 2 0x71764129 0 100 0', timeout=10)   # continuous
dumps = {}
for st in ('forced', 'agc'):
    for i in (0, 1):
        rx = rfhost.Board(rfhost.C948)
        if st == 'forced': rx.cmd('forced 1')
        lines = rx.cmd('rxdump 2', timeout=90); rx.s.close()
        open(os.path.join(d, 'coded_%s_%d.txt' % (st, i)), 'w').write('\n'.join(lines) + '\n')
        dumps[st, i] = {tuple(l.split()[:-1]): l.split()[-1] for l in lines if l[:2] in ('r ', 'a ')}
tx.cmd('stop')
stable = lambda s: {k: v for k, v in dumps[s, 0].items() if dumps[s, 1].get(k) == v}
f, a = stable('forced'), stable('agc')
for k in sorted(set(f) & set(a)):
    if f[k] != a[k]: print(' '.join(k), 'forced', f[k], 'agc', a[k])
