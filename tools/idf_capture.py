#!/usr/bin/env python3
"""Reset C9:48 (IDF app already flashed), optionally start a software BLE advertiser on D0 (rfctl bleadv),
and print C9:48's console until '=== done'.  idf_capture.py [--adv CHAN] [--timeout S]"""
import argparse, time, serial, rfhost

LO = {37: 2400000, 38: 2424000, 39: 2478000}
a = argparse.ArgumentParser(); a.add_argument('--adv', type=int); a.add_argument('--timeout', type=float, default=60)
o = a.parse_args()
if o.adv:
    tx = rfhost.Board(rfhost.D0); tx.cmd('init'); tx.cmd('tunetx %d' % LO[o.adv]); tx.cmd('bleadv %d 2000 300 NDF' % o.adv)
s = serial.Serial(None, 115200, timeout=0.5)
s.port = rfhost.C948; s.dtr = False; s.rts = False; s.open()   # DTR low: no download mode on the CH340 boards
s.rts = True; time.sleep(0.1); s.rts = False
end, buf = time.time() + o.timeout, b''
while time.time() < end and b'=== done' not in buf:
    buf += s.read(4096)
t = buf.decode(errors='replace')
print('\n'.join(l for l in t.splitlines() if not l.startswith(('\x1b', 'I (', 'W ('))))
