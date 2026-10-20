#!/usr/bin/env python3
"""Intermittent TX: repeat sessions, record tone and D0 TX-side state."""
from rfhost import *
regs = [0x60006000, 0x60006110, 0x60006104, 0x60006118, 0x6000611C, 0x60006120, 0x60006124, 0x6002600C, 0x6001C02C,
        0x60033D64, 0x600C101C, 0x6000607C, 0x60006040, 0x600061E4, 0x60006048, 0x600060E0, 0x60008034, 0x60026014]
ana = [(0x6B, r) for r in (1, 2, 3, 4, 5, 6, 7, 8, 0xA, 0xB)] + [(0x62, 0xC), (0x62, 1), (0x67, 0)]
res = []
for run in range(8):
    rx = Board(C948); tx = Board(D0)
    rx.cmd('init'); tx.cmd('init'); rx.cmd('rxcal')
    t = tx.cmd('tunetx 2437000')[0]; rx.cmd('tune 2436000'); rx.cmd('gain 20')
    tx.cmd('tx 127 0 2000 300')
    tone = rx.meas(8192, 3e6)['bins'][0]
    st = {('%08x' % a): tx.cmd('rd 0x%x' % a)[0].split()[1] for a in regs}
    st.update({('a%02x%02x' % (b, r)): tx.cmd('ard 0x%x 0x%x' % (b, r))[0].split()[1] for b, r in ana})
    st['row160'] = ' '.join(tx.cmd('rowget 160')[0].split()[2:])
    tx.cmd('txoff'); rx.s.close(); tx.s.close()
    print('run %d tone %.1f %s' % (run, tone, t)); res.append((tone, st))
good = [s for t, s in res if t > 20]; bad = [s for t, s in res if t < 10]
print(len(good), 'good', len(bad), 'bad')
if good and bad:
    for k in good[0]:
        gv = {g[k] for g in good}; bv = {b[k] for b in bad}
        if gv != bv: print(k, 'good', gv, 'bad', bv)
