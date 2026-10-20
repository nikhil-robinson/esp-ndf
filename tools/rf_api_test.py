#!/usr/bin/env python3
"""Drive examples/rf_api_test on C9:48 with D0 (rfctl_uart) as the other end; print the test's lines and D0's measurements."""
import serial, time, rfhost
d = rfhost.Board(rfhost.D0); d.cmd('init')
c = serial.Serial(None, 115200, timeout=0.2); c.port = rfhost.C948; c.dtr = False; c.rts = False; c.open()
c.rts = True; time.sleep(0.1); c.rts = False
buf, end = b'', time.time() + 120
def lines():
    global buf
    buf += c.read(4096)
    while b'\n' in buf:
        l, buf = buf.split(b'\n', 1); yield l.decode(errors='replace').strip()
done = False
while time.time() < end and not done:
    for l in lines():
        if l.startswith(('PASS', 'FAIL', 'rx ', 'hop', '===')): print(l)
        if l == 'STEP 1':
            d.cmd('tunetx 2437000'); d.cmd('tx 127 0 2000 300'); time.sleep(0.2); c.write(b'g')
        elif l == 'STEP 2':
            d.cmd('txoff'); d.cmd('rxcal'); d.cmd('tune 2436000'); d.cmd('gain 10'); time.sleep(0.1)
            m = d.meas(8192, 2e6, 6e6); print('D0 sees tone %.1f dB, floor %.1f dB' % (m['bins'][0], m['bins'][1])); full = m['bins'][0]; c.write(b'g')
        elif l == 'STEP 3':
            time.sleep(0.1); m = d.meas(8192, 2e6, 6e6); print('D0 sees tone %.1f dB at -10 dB level (step %.1f dB)' % (m['bins'][0], m['bins'][0] - full)); c.write(b'g')
        elif l.startswith('=== done'): done = True
