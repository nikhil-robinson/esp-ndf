#!/usr/bin/env python3
"""Host side of examples/usb_safety_proof: arm, wait through the RTC-watchdog reset, check the RTC_CNTL bit came back
cleared and the console returned. Usage: usb_safety_proof.py PORT"""
import serial, sys, time
port = sys.argv[1]
c = serial.Serial(); c.port = port; c.baudrate = 115200; c.timeout = 0.3; c.dtr = False; c.rts = False; c.open()
time.sleep(0.5); c.reset_input_buffer()
c.write(b'g')
t, buf = time.time(), b''
while time.time() - t < 12:                        # through the reset: the port drops and comes back
    try:
        if not c.is_open: c.open()
        buf += c.read(4096)
        if b'armed' in buf and b'boot' in buf.split(b'armed', 1)[1]: break
    except Exception:
        try: c.close()
        except Exception: pass
        time.sleep(0.2)
out = buf.decode(errors='replace')
after = out.split('armed', 1)[-1]
ok = 'armed, PAD_HOLD bit 21 = 1' in out and 'boot, PAD_HOLD bit 21 = 0' in after
print(out.strip()[-400:])
print('=== usb_safety_proof %s ===' % ('PASS: RTC reset cleared RTC_CNTL, console back' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
