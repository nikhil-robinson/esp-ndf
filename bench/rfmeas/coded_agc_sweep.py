#!/usr/bin/env python3
"""Coded (S8) with the hardware AGC receives nothing; held gain 42 receives. Sweep every bit of the AGC block
0x6001C000..0x6001C0FC (set, then clear), applied after rx start, 300 ms receive per case, D0 sending S8
continuously. Prints cases that receive. Word restored after each case; a hang resets the board.
Args: [first_off last_off]. Run from tools/ with PYTHONPATH=."""
import rfhost, sys, time
lo, hi = (int(sys.argv[1], 0), int(sys.argv[2], 0)) if len(sys.argv) > 2 else (0, 0xFC)
tx = rfhost.Board(rfhost.D0); tx.cmd('send 2404 2 0x71764129 0 100 0', timeout=10)
rx = rfhost.Board(rfhost.C948)
def rxgood():
    l = [x for x in rx.cmd('rx 2404 2 0x71764129 0 -1 300', timeout=8) if x.startswith('rx rc')][0].split()
    return int(l[4]), int(l[6])
rx.cmd('rx 2404 2 0x71764129 0 -1 20'); print('baseline', rxgood(), flush=True)
for off in range(lo, hi + 4, 4):
    a = 0x6001C000 + off
    for b in range(32):
        for mode in ('set', 'clr'):
            try:
                orig = int(rx.cmd('rd %#x' % a)[0].split()[2], 16)
                if mode == 'set' and orig >> b & 1 or mode == 'clr' and not orig >> b & 1: continue
                rx.cmd('rxset 0 0 0'); rx.cmd('rxset %#x %#x %#x' % ((a, 0, 1 << b) if mode == 'set' else (a, 1 << b, 0)))
                g, c = rxgood(); rx.cmd('rxset 0 0 0'); rx.cmd('wr %#x %#x' % (a, orig))
                if g or c: print('%#x bit %2d %s: good %d crcbad %d' % (a, b, mode, g, c), flush=True)
            except Exception as e:
                print('%#x bit %2d %s: HANG (%s), reset' % (a, b, mode, str(e)[:40]), flush=True)
                rx.s.close(); rx = rfhost.Board(rfhost.C948); rx.cmd('rx 2404 2 0x71764129 0 -1 20')
    print('# done %#x' % a, flush=True)
tx.cmd('stop')
