#!/usr/bin/env python3
"""Format 0x03 RX in hardware mode (AGC on, rf_modem rawrx, no forced front end): sweep the reply TX power index
(CS+0x18 low byte) and count the replies D0 hears. Goal: an index that radiates nothing. Run from tools/ with PYTHONPATH=."""
import rfhost, threading, time
for pw in (0x0b, 0x00, 0x01, 0x0f, 0x10, 0x1f, 0x20, 0x3f, 0x40, 0x7f, 0x80, 0xff):
    tx = rfhost.Board(rfhost.D0); rx = rfhost.Board(rfhost.C948); rx.cmd('init'); res = {}
    th = threading.Thread(target=lambda: res.setdefault('rx', rx.cmd('rawrx 17 0 0x71764129 2500 0:0 2 3 %#x' % (0xE000 | pw), timeout=20))); th.start(); time.sleep(0.8)
    # D0: count what its master events hear while sending (rf_pkt RX ring is read by 'heard' in rf_modem only) -> use events/s as proxy
    t = tx.cmd('init') and tx.cmd('rawsend 17 0 0x71764129 251 2', timeout=10)[0]   # rf_modem on D0: prints events/s and replies heard
    th.join(); out = res['rx']
    print('txpwr %#04x: D0 %s | %s | %s' % (pw, t, out[-2].replace('rawrx err bits: ', ''), out[-1].split(' core374')[0].replace('rawrx rc 0 ', '')), flush=True)
