#!/usr/bin/env python3
"""Apply groups of vendor-BLE analog values (rx_analog_diff.txt) on the TRANSMITTER (D0), then rf_pkt rx: 1000 x 200 B 1M 2460 MHz
at TX power index 5 (sensitivity) and 11 (default). Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time
groups = {
  'none': [],
  'pll loop 62.08-0a 63.01': [('62', '08', '00'), ('62', '09', '02'), ('62', '0a', 'f0'), ('63', '01', 'f3')],
  '6b block': [('6b', '%02x' % r, v) for r, v in ((1, '17'), (2, '3b'), (3, '08'), (4, 'a4'), (5, '0a'), (6, '08'), (7, '5f'), (8, '0a'), (0xa, '74'), (0xb, '05'))],
  '61.04-06': [('61', '04', '65'), ('61', '05', '47'), ('61', '06', '61')],
  'rx filter 67': [('67', '04', '1e'), ('67', '05', '1e'), ('67', '0c', '1e'), ('67', '0d', '1e')],
  '66.08 6d': [('66', '08', 'c3'), ('6d', '04', '9b'), ('6d', '05', '17'), ('6d', '06', '9e'), ('6d', '0d', '42')],
}
for name, ws in groups.items():
  for pw in (5, 11):
    tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); tx.cmd('txp %d' % pw); res = {}
    tx.cmd('send 2460 0 0x71764129 0 200 1', timeout=10)            # set up the core first
    for b, r, v in ws: tx.cmd('awr %s %s %s' % (b, r, v))
    th = threading.Thread(target=lambda: res.setdefault('rx', [l for l in rx.cmd('rx 2460 0 0x71764129 0 -1 3500', timeout=30) if l.startswith('rx rc')][0])); th.start(); time.sleep(0.3)
    tx.cmd('send 2460 0 0x71764129 0 200 1000', timeout=20); th.join()
    print('%-26s power %2d: %s' % (name, pw, res['rx']), flush=True)
