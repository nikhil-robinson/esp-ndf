#!/usr/bin/env python3
"""Print the console of the app on the USB-JTAG port until a line containing the end marker (default 'done').
usage: run_app.py PORT [marker] [timeout_s]. The chip is reset with esptool's watchdog reset (a reset through the
DTR/RTS lines can land in download mode when the host leaves DTR asserted, e.g. ModemManager), then read without
touching DTR/RTS, so output starts at boot.
A reset that drops the USB device (chip/RTC reset) is followed: the port is reopened without touching DTR/RTS."""
import sys, os, time, glob, serial, subprocess
port, mark, tmo = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else 'done'), float(sys.argv[3]) if len(sys.argv) > 3 else 20
subprocess.run(['esptool.py', '--chip', os.environ.get('NDF_CHIP', 'esp32s3'), '-p', port, '--after', os.environ.get('NDF_AFTER', 'watchdog_reset'), 'chip_id'],      # NDF_CHIP=esp32p4 NDF_AFTER=hard_reset for the P4 on its UART bridge

               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
for _ in range(50):                                        # the port comes back after the reset
    try:
        s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.2; s.dtr = False; s.rts = False; s.open(); break
    except (serial.SerialException, OSError):
        time.sleep(0.1)
t = time.time(); buf = b''
while time.time() - t < tmo:
    try:
        buf += s.read(4096)
    except (serial.SerialException, OSError):
        s.close()
        while time.time() - t < tmo:                       # wait for the device to come back
            time.sleep(0.2)
            ports = [port] if port.startswith('/dev/serial/by-id/') else sorted(glob.glob('/dev/ttyACM*'))   # same board: its by-id link returns
            ports = [p for p in ports if os.path.exists(p)]
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
