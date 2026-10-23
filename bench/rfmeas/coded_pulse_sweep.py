#!/usr/bin/env python3
"""Dead receive state (AGC free-running through a coded window timeout) stays dead until rf_open + ble_hw_init.
Look for the single bit that restarts it: in the dead state, pulse each bit (flip, restore) of a register block and
run a 200 ms S8 window; a window with packets = revive (logged), then the receiver is killed again by a silent
window. No reboot between cases. Args: base length (default AGC block 0x6001C000 0x100). Run from tools/ with PYTHONPATH=."""
import rfhost, sys
base, n = (int(sys.argv[1], 0), int(sys.argv[2], 0)) if len(sys.argv) > 2 else (0x6001C000, 0x100)
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948)
win = lambda ms=200: int([x for x in rx.cmd('rx 2404 2 0x71764129 0 -1 %d' % ms, timeout=8) if x.startswith('rx rc')][0].split()[4])
def kill():                                       # silent window = timeout with the AGC running; then the sender back on
    tx.cmd('stop'); win(100); tx.cmd('send 2404 2 0x71764129 0 100 0', timeout=10)
    return win() == 0 and win() == 0
print('dead:', kill(), flush=True)
for a in range(base, base + n, 4):
    for b in range(32):
        try:
            v = int(rx.cmd('rd %#x' % a)[0].split()[2], 16)
            rx.cmd('wr %#x %#x' % (a, v ^ 1 << b)); rx.cmd('wr %#x %#x' % (a, v))
            g = win()
            if g: print('%#x bit %2d pulse (was %d): REVIVE %d, re-dead %s' % (a, b, v >> b & 1, g, kill()), flush=True)
        except Exception as e:
            print('%#x bit %2d: HANG (%s), reset' % (a, b, str(e)[:40]), flush=True)
            rx.s.close(); rx = rfhost.Board(rfhost.C948); print('  dead again:', kill(), flush=True)
    print('# %#x' % a, flush=True)
tx.cmd('stop')
