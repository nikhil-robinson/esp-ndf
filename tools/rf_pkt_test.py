#!/usr/bin/env python3
"""Two-board test of the hardware packet engine (examples/rf_pkt on both): D0 sends, C9:48 receives.
Every case checks: packets arrive, no wrong byte, length/header right; plus negative cases (whitening / CRC mismatch,
out-of-band send denied, counted send exact). Prints PASS/FAIL lines and a summary.  PYTHONPATH=. python3 rf_pkt_test.py"""
import rfhost, threading, time
fails = 0
def check(name, cond, info=''):
    global fails
    print('%s %s  %s' % ('PASS' if cond else 'FAIL', name, info), flush=True); fails += 0 if cond else 1

def link(mhz, rate, sync, len_, txflags=0, rxflags=0, gain=127, ms=1500):
    tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948)
    res = {}
    th = threading.Thread(target=lambda: res.setdefault('rx', [l for l in rx.cmd('rx %d %d %s %d %d %d' % (mhz, rate, sync, rxflags, gain, ms), timeout=30) if l.startswith('rx rc')][0])); th.start()
    time.sleep(0.3)
    t = tx.cmd('send %d %d %s %d %d 0' % (mhz, rate, sync, txflags, len_), timeout=10)[0].split(' ctl')[0]
    th.join()
    r = res['rx'].split()
    d = dict(zip(r[1::2], r[2::2]))
    d.pop('ctl', None)
    return t, {k: int(v, 16) if k == 'hdr' else int(v) for k, v in d.items()}

for mhz, rate, sync, ln in ((2402, 0, '0x71764129', 37), (2420, 0, '0x12345678', 100), (2460, 0, '0x12345678', 100), (2440, 0, '0x12345678', 100), (2480, 0, '0xA5A5F00F', 255), (2426, 1, '0x71764129', 255),
                            (2404, 2, '0x71764129', 100), (2478, 3, '0x71764129', 255), (2440, 0, '0x71764129', 1)):
    t, r = link(mhz, rate, sync, ln)
    check('%d MHz rate %d sync %s len %d' % (mhz, rate, sync, ln), r['good'] > 50 and r['wrong'] == 0 and r['len'] == ln and r['hdr'] == 0x5A and r['crcbad'] * 20 < r['good'], '%s | %s' % (t, r))
for txf, rxf, expect in ((1, 1, 'good'), (1, 0, 'crcbad'), (0, 1, 'good')):
    t, r = link(2440, 0, '0x71764129', 37, txf, rxf)
    ok = r['good'] > 50 and r['wrong'] == 0 if expect == 'good' else (r['good'] == 0 and r['crcbad_dataok'] > 50)
    check('CRC flag tx %d rx %d (1 = CRC off) -> %s' % (txf, rxf, expect), ok, '%s | %s' % (t, r))
tx = rfhost.Board(rfhost.D0)
check('whitening off refused (not supported yet)', tx.cmd('send 2440 0 0x71764129 2 37 0')[0] == 'send rc -2')
tx = rfhost.Board(rfhost.D0)
check('send at 2500 MHz denied', tx.cmd('send 2500 0 0x71764129 0 37 0')[0].startswith('send rc -2'), '(range)')
tx.cmd('limits 2400000 2420000')
check('send at 2440 MHz outside limits denied', tx.cmd('send 2440 0 0x71764129 0 37 0')[0] == 'send rc -4')
tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); res = {}
th = threading.Thread(target=lambda: res.setdefault('rx', [l for l in rx.cmd('rx 2440 0 0x71764129 0 127 2500', timeout=30) if l.startswith('rx rc')][0])); th.start(); time.sleep(0.3)
t = tx.cmd('send 2440 0 0x71764129 0 37 1000', timeout=20)[0]; th.join()
got = int(res['rx'].split()[4])
check('counted send: 1000 packets sent, >= 990 received', 'sent 1000' in t and 990 <= got <= 1000, '%s | %s' % (t, res['rx']))
print('=== %d failures' % fails)
