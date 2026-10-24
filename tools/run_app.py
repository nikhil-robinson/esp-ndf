#!/usr/bin/env python3
"""Print the console of the app on the USB-JTAG port until a line containing the end marker (default 'done').
usage: run_app.py PORT [marker] [timeout_s]. Opening the port resets the chip (DTR/RTS), so output starts at boot.
A reset that drops the USB device (chip/RTC reset) is followed: the port is reopened without touching DTR/RTS."""
import sys, time, glob, serial
port, mark, tmo = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else 'done'), float(sys.argv[3]) if len(sys.argv) > 3 else 20
s = serial.Serial(port, 115200, timeout=0.2); s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
t = time.time(); buf = b''
while time.time() - t < tmo:
    try:
        buf += s.read(4096)
    except (serial.SerialException, OSError):
        s.close()
        while time.time() - t < tmo:                       # wait for the device to come back
            time.sleep(0.2)
            ports = sorted(glob.glob('/dev/ttyACM*'))
            if not ports: continue
            try:
                s = serial.Serial(); s.port = ports[0]; s.baudrate = 115200; s.timeout = 0.2; s.dtr = False; s.rts = False; s.open(); break
            except (serial.SerialException, OSError):
                continue
        continue
    while b'\n' in buf:
        line, buf = buf.split(b'\n', 1); l = line.decode(errors='replace').rstrip(); print(l, flush=True)
        if mark in l: sys.exit(0)
sys.exit(1)
