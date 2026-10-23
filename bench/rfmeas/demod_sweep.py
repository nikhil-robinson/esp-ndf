#!/usr/bin/env python3
"""Demodulator search: D0 loops GFSK 'rate:dev' (default 1000:100 = syncs, CRC fails at the default setting). Every bit
of a block (default modem 0x60011000..0x600110FC) set / cleared after rx start (rxset), 200 ms receive; a case with
good packets is printed. Word restored after each case; a hang resets the board. Args: rate dev [base len].
Run from tools/ with PYTHONPATH=."""
import rfhost, sys
rate, dev = sys.argv[1], sys.argv[2]
base, n = (int(sys.argv[3], 0), int(sys.argv[4], 0)) if len(sys.argv) > 4 else (0x60011000, 0x100)
tx = rfhost.Board(rfhost.D0); print(tx.cmd('gfsk 2440 %s %s 20 200 100' % (rate, dev), timeout=30))
rx = rfhost.Board(rfhost.C948)
def win():
    l = [x for x in rx.cmd('rx 2440 0 0x71764129 0 -1 200', timeout=8) if x.startswith('rx rc')][0].split()
    return int(l[4]), int(l[6])
print('baseline', win(), flush=True)
for a in range(base, base + n, 4):
    for b in range(32):
        for mode in ('set', 'clr'):
            try:
                v = int(rx.cmd('rd %#x' % a)[0].split()[2], 16)
                if (mode == 'set') == bool(v >> b & 1): continue
                rx.cmd('rxset 0 0 0'); rx.cmd('rxset %#x %#x %#x' % ((a, 0, 1 << b) if mode == 'set' else (a, 1 << b, 0)))
                g, c = win(); rx.cmd('rxset 0 0 0'); rx.cmd('wr %#x %#x' % (a, v))
                if g > 5: print('%#x bit %2d %s: good %d crcbad %d' % (a, b, mode, g, c), flush=True)
            except Exception as e:
                print('%#x bit %2d %s: HANG (%s), reset' % (a, b, mode, str(e)[:40]), flush=True)
                rx.s.close(); rx = rfhost.Board(rfhost.C948)
    print('# %#x' % a, flush=True)
tx.cmd('stop')
