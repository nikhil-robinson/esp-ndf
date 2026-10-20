#!/usr/bin/env python3
"""Format 0x03 RX: flip single bits of CS+0x02 (0x0801), +0x04 (0x1100), +0x18 (0xE000) looking for a 'no reply' bit:
D0 (raw-sending 251 B) hears nothing while C9:48 still receives. Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time
cases = [(0x02, 0x0801 ^ (1 << b)) for b in range(16)] + [(0x04, 0x1100 ^ (1 << b)) for b in range(4, 16)] + [(0x18, 0xE000 ^ (1 << b)) for b in range(8, 13)]
for off, val in cases:
    tx = rfhost.Board(rfhost.D0); tx.cmd('init')
    rx = rfhost.Board(rfhost.C948); rx.cmd('init')
    res = {}
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rawrx 37 0 0x71764129 2200 %#x:%#x 2 3' % (off, val), timeout=20))); th.start()
    time.sleep(0.6)
    try: a = tx.cmd('rawsend 37 0 0x71764129 251 2', timeout=10)[0]
    except Exception: a = 'tx?'
    th.join(timeout=25); out = res.get('rx', ['-', '-'])
    mark = '  <==' if ' heard 0' in a and ' ok 0 ' not in out[-1] else ''
    print('CS+%#04x=%#06x: D0 %s | %s%s' % (off, val, a.replace('rawsend rc 0 ', ''), out[-1].split(' core374')[0].replace('rawrx rc 0 ', ''), mark), flush=True)
