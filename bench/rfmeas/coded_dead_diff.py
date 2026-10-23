#!/usr/bin/env python3
"""Receive goes dead after a few windows (all rates, until reboot). Per boot: rxdump 2 right after boot (good), rx
windows until two empty in a row, rxdump 2 again (dead). Diff kept only if it repeats on every boot.
Output: data/dead_{good,bad}_<boot>.txt. Run from tools/ with PYTHONPATH=."""
import rfhost, os
d = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')
tx = rfhost.Board(rfhost.D0); tx.cmd('send 2404 2 0x71764129 0 100 0', timeout=10)
win = lambda rx: int([x for x in rx.cmd('rx 2404 2 0x71764129 0 -1 300', timeout=8) if x.startswith('rx rc')][0].split()[4])
parse = lambda L: {tuple(l.split()[:-1]): l.split()[-1] for l in L if l[:2] in ('r ', 'a ', 'e ')}
diffs = []
for boot in range(3):
    rx = rfhost.Board(rfhost.C948); g0 = win(rx)
    good = rx.cmd('rxdump 2', timeout=90); seq = [g0, win(rx)]
    while len(seq) < 30 and not (seq[-1] == 0 and seq[-2] == 0): seq.append(win(rx))
    bad = rx.cmd('rxdump 2', timeout=90); after = win(rx)
    print('boot %d windows %s, after bad dump %d' % (boot, seq, after), flush=True)
    open(os.path.join(d, 'dead_good_%d.txt' % boot), 'w').write('\n'.join(good) + '\n')
    open(os.path.join(d, 'dead_bad_%d.txt' % boot), 'w').write('\n'.join(bad) + '\n')
    G, B = parse(good), parse(bad); diffs.append({k: (G[k], B[k]) for k in G if k in B and G[k] != B[k]})
    rx.s.close()
tx.cmd('stop')
for k in sorted(set.intersection(*[set(x) for x in diffs])):
    print(' '.join(k), ' | '.join('%s->%s' % x[k] for x in diffs))
