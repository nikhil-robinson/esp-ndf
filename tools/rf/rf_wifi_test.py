#!/usr/bin/env python3
"""4.2 firmware-only path: C9:48 (ttyUSB0) rx, D0 (ttyUSB1) tx; both run examples/rf/rf_wifi."""
import time, serial
def mk(p): s=serial.Serial(None,115200,timeout=0.1); s.port=p; s.dtr=False; s.rts=False; s.open(); time.sleep(1.5); s.reset_input_buffer(); return s
rx=mk('/dev/ttyUSB0'); tx=mk('/dev/ttyUSB1')
tx.write(b'tx 0\n'); time.sleep(2.0)
rx.write(b'rx 6000\n'); end=time.time()+9
while time.time()<end:
    l=rx.readline().decode(errors='replace').strip()
    if l.startswith(('rx len','rx init')): print('RX:', l)
rx.close(); tx.close()
