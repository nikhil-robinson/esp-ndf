#!/usr/bin/env python3
"""Demodulator words per rate: dump modem 0x60011000..FC and 0x6001CC00..FC while receiving at each engine rate (1M, 2M, S8, S2),
then for every word that differs between rates force each rate's value (rxset after rx start) while D0 loops GFSK 1000:100
(syncs, CRC fails at default). Prints the dumps' differences and any forced value that gives good packets. Run from tools/, PYTHONPATH=.
Data: bench/rfmeas/data/demod_ccdiff.txt"""
import rfhost
rx = rfhost.Board(rfhost.C948)
blocks = [(0x60011000, 64), (0x6001CC00, 64)]
def dump():
    out = {}
    for base, n in blocks:
        for a in range(base, base + 4 * n, 4): out[a] = int(rx.cmd('rd %#x' % a)[0].split()[2], 16)
    return out
dumps = {}
for eng in range(4):
    rx.cmd('rxset 0 0 0'); rx.cmd('rx 2440 %d 0x71764129 0 -1 30' % eng, timeout=8)   # leaves the receiver configured for this rate
    dumps[eng] = dump()
diff = [a for a in dumps[0] if len({dumps[e][a] for e in dumps}) > 1]
for a in diff: print('%#x ' % a + ' '.join('%d:%08x' % (e, dumps[e][a]) for e in dumps), flush=True)
tx = rfhost.Board(rfhost.D0); print(tx.cmd('gfsk 2440 1000 100 20 200 100', timeout=30))
def win(extra):
    rx.cmd('rxset 0 0 0')
    for a, v in extra: rx.cmd('rxset %#x 0xffffffff %#x' % (a, v))
    l = [x for x in rx.cmd('rx 2440 0 0x71764129 0 -1 200', timeout=8) if x.startswith('rx rc')][0].split()
    return int(l[4]), int(l[6])
print('baseline 1M dev 100:', win([]), flush=True)
for e in (1, 2, 3):
    cc = [(a, dumps[e][a]) for a in diff if a >= 0x6001CC00 and dumps[e][a] != dumps[0][a]][:8]   # rf_pkt keeps up to 8 rxset words
    print('differing 0x6001CCxx words from rate %d (first 8):' % e, win(cc), flush=True)
    for a in diff:
        if dumps[e][a] != dumps[0][a]: print('  %#x = %08x: good %d crcbad %d' % ((a, dumps[e][a]) + win([(a, dumps[e][a])])), flush=True)
rx.cmd('rxset 0 0 0'); tx.cmd('stop')
