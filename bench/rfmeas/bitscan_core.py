#!/usr/bin/env python3
"""Resumable bit-flip scan of the BLE core registers (examples/rf_modem 'bitscan'): resets and skips words whose bit flip hangs the chip."""
import rfhost, time
tx = rfhost.Board(rfhost.D0); tx.cmd('init'); tx.cmd('adv 37 2000')
start, end, hangs = 0x60031000, 0x60031000 + 4 * 192, []
while start < end:
    rx = rfhost.Board(rfhost.C948); rx.cmd('init'); rx.cmd('rx 2402000')
    rx.s.write(('bitscan %#x %d\n' % (start, (end - start) // 4)).encode())
    buf, last, t = b'', start, time.time()
    while time.time() - t < 15:                        # no progress for 15 s = hung
        d = rx.s.read(4096)
        if d: t = time.time(); buf += d
        lines = buf.decode(errors='replace').split('\n')
        for l in lines:
            if l.startswith('w '): last = int(l.split()[1], 16)
        if 'bitscan hits' in buf.decode(errors='replace'): break
    for l in buf.decode(errors='replace').split('\n'):
        if l.startswith(('hit', 'bitscan')): print(l.strip())
    if b'bitscan hits' in buf: break
    print('hang at word %#x' % last, flush=True); hangs.append(last); start = last + 4
print('# words whose bit flip hangs the chip:', ' '.join('%#x' % w for w in hangs))
