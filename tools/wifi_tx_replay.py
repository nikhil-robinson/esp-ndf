#!/usr/bin/env python3
"""4.2: replay the captured Wi-Fi MAC register snapshot into D0 (rf_pkt), then fire one
frame through the MAC TX descriptor path and report the TX state-machine status.

Baseband/FE are not in the snapshot as absolute values (only deltas), so this drives the
MAC side only: the milestone here is the MAC accepting the descriptor and advancing.
Run bench/idf_wifisniff on C9:48 to see if anything is received."""
import sys, time, serial

PORT = '/dev/ttyUSB1'                     # D0
STATE = 'bench/idf_phyble/wifi_tx_state2.txt'   # MAC + baseband (0x6001c000) + FE (0x60006000)
# firmware (wifi_tx_fire) drives these; don't overwrite from the snapshot
SKIP = {0x60033c34, 0x60033c3c, 0x60033c40, 0x60033d04, 0x60033d08, 0x600342f8, 0x60034300}
# write baseband + FE first, then the MAC
RANGES = [(0x60006000, 0x60006200), (0x6001c000, 0x6001e000), (0x60033000, 0x60035000)]

def regs():
    vals = {}
    for ln in open(STATE):
        if ln.startswith('r '):
            _, a, v = ln.split(); vals[int(a, 16)] = int(v, 16)
    for lo, hi in RANGES:
        for a in range(lo, hi, 4):
            v = vals.get(a, 0)
            if v != 0 and a not in SKIP:
                yield a, v

s = serial.Serial(None, 115200, timeout=0.3)
s.port = PORT; s.dtr = False; s.rts = False; s.open()
time.sleep(0.2); s.reset_input_buffer()

def cmd(line, want='ok', t=1.0):
    s.write((line + '\n').encode()); end = time.time() + t; out = []
    while time.time() < end:
        l = s.readline().decode(errors='replace').strip()
        if l: out.append(l)
        if l == want: break
    return out

print('wup:', [l for l in cmd('wup', t=2.0) if l.startswith('wup')])
n = 0
for a, v in regs():
    cmd('wr 0x%08x 0x%08x' % (a, v)); n += 1
print('replayed %d MAC registers' % n)
for l in cmd('wtx'):
    if l.startswith('wtx'): print(l)
print('d08/c3c after:', [l for l in cmd('rd 0x60033c3c 1') if l.startswith('r ')])

# keep the SAME session (config persists) and fire continuously for 15 s
import time
print('firing 15 s ...'); s.timeout = 0.02; end = time.time() + 15; k = 0
while time.time() < end:
    s.write(b'wtx\n'); k += 1
    if k % 50 == 0: s.reset_input_buffer()
print('fired', k)
s.close()
