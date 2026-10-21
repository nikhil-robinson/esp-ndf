#!/usr/bin/env python3
"""Apply one vendor-BLE-scan register value (bench/rfmeas/rx_state_diff.txt) on the receiver before rf_pkt rx and measure
sensitivity: D0 at TX power index 5 (~-64 dBm here), 1000 x 200 B at 1M 2460 MHz. Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time
cands = [None, ('0x6001c02c', '0x364052ed'), ('0x6001c084', '0x02b89ff4'), ('0x6001c08c', '0x03fff9ff'), ('0x6001c090', '0xd8ab2a0a'),
         ('0x6001d008', '0x00003000'), ('0x6001d048', '0x6'), ('0x6001d04c', '0x0'), ('0x6001d090', '0xfdf01e00'), ('0x6000e05c', '0xe000016a'),
         ('0x6000e170', '0x007c0000'), ('0x6000e168', '0x3e001700'), ('0x6000607c', '0xff7d1f40'), ('0x600060c0', '0xb0f07813'), ('0x600060c4', '0x00005040')]
for c in cands:
    tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); tx.cmd('txp 5'); res = {}
    rx.cmd('rx 2460 0 0x71764129 0 -1 5', timeout=10)               # set up the core first
    if c: rx.cmd('wr %s %s' % c)
    th = threading.Thread(target=lambda: res.setdefault('rx', [l for l in rx.cmd('rx 2460 0 0x71764129 0 -1 3500', timeout=30) if l.startswith('rx rc')][0])); th.start(); time.sleep(0.3)
    t = tx.cmd('send 2460 0 0x71764129 0 200 1000', timeout=20)[0]; th.join()
    print('%s: %s' % (c, res['rx']), flush=True)
