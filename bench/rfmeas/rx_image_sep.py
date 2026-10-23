#!/usr/bin/env python3
"""RX image rejection without D0's TX image. D0 tone at LO_t + 1 MHz; C9:48 at LO_t - 2 MHz: tone at +3 MHz baseband,
our RX image at -3 MHz, D0's TX image at +1 MHz (no overlap). 'same' = old setup (RX LO = TX LO, tone +1 MHz:
TX image lands on the RX image). Run from tools/ with PYTHONPATH=."""
import rfhost
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948)
for mhz in (2404, 2420, 2440, 2460, 2478):
    tx.cmd('tone %d 1000' % (mhz * 1000), timeout=10)
    sep = rx.cmd('image %d 3000000' % ((mhz - 2) * 1000), timeout=20)[-1]
    same = rx.cmd('image %d 1000000' % (mhz * 1000), timeout=20)[-1]
    print('%d MHz  separated: %s  |  same LO: %s' % (mhz, sep, same), flush=True)
tx.cmd('stop')
