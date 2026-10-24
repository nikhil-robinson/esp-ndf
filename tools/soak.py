#!/usr/bin/env python3
"""Soak logger: follows a board's console (across resets / USB re-enumeration), timestamps every line into
bench/soak/data/<name>.log and prints a summary line per minute. Usage: soak.py PORT NAME HOURS"""
import serial, sys, time, os
port, name, hours = sys.argv[1], sys.argv[2], float(sys.argv[3])
log = open(os.path.join(os.path.dirname(__file__), '..', 'bench', 'soak', 'data', name + '.log'), 'a')
end, last_sum, rounds, fails, boots, crashes, buf = time.time() + hours * 3600, 0, 0, 0, 0, 0, b''
c = None
while time.time() < end:
    try:
        if c is None:
            c = serial.Serial(); c.port = port; c.baudrate = 115200; c.timeout = 1; c.dtr = False; c.rts = False; c.open()
        buf += c.read(4096)
    except Exception:
        try: c.close()
        except Exception: pass
        c = None; time.sleep(1); continue
    while b'\n' in buf:
        line, buf = buf.split(b'\n', 1)
        s = line.decode(errors='replace').strip()
        if not s: continue
        log.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), s)); log.flush()
        if 'round' in s: rounds += 1; fails += ' FAIL ' in s
        if 'soak: boot' in s: boots += 1
        if 'CRASH' in s or 'PANIC' in s: crashes += 1
    if time.time() - last_sum > 60:
        last_sum = time.time()
        print('%s rounds %d fails %d boots %d crashes %d' % (time.strftime('%H:%M:%S'), rounds, fails, boots, crashes), flush=True)
print('=== soak %s: rounds %d fails %d boots %d crashes %d ===' % (name, rounds, fails, boots, crashes))
