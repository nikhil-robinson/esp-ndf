#!/usr/bin/env python3
"""4.2 receive: replay the captured Wi-Fi RX config onto C9:48 (rf_pkt), set up our own RX
descriptor ring, and report frames the MAC DMAs into our buffers. One serial session."""
import sys, time, serial
PORT = '/dev/ttyUSB0'                     # C9:48
STATE = 'bench/idf_wifirx/wifi_rx_state.txt'
SKIP = {0x60033088, 0x6003308c, 0x60033090, 0x60033094, 0x60033098,   # RX ring heads (wrx sets)
        0x60033c34, 0x60033c3c, 0x60033c40}
RANGES = [(0x60006000, 0x60006200), (0x6001c000, 0x6001e000), (0x60033000, 0x60035000)]

vals = {}
for ln in open(STATE):
    if ln.startswith('r '):
        _, a, v = ln.split(); vals[int(a,16)] = int(v,16)

s = serial.Serial(None,115200,timeout=0.3); s.port=PORT; s.dtr=False; s.rts=False; s.open()
time.sleep(0.2); s.reset_input_buffer()
def cmd(line, want='ok', t=1.0):
    s.write((line+'\n').encode()); end=time.time()+t; out=[]
    while time.time()<end:
        l=s.readline().decode(errors='replace').strip()
        if l: out.append(l)
        if l==want: break
    return out

print('wrxup:', [l for l in cmd('wrxup', t=2.0) if l.startswith('wrxup')])
n=0
for lo,hi in RANGES:
    for a in range(lo,hi,4):
        v=vals.get(a,0)
        if v!=0 and a not in SKIP:
            cmd('wr 0x%08x 0x%08x'%(a,v)); n+=1
print('replayed',n,'RX registers')
for l in cmd('wrx %d'%(int(sys.argv[1]) if len(sys.argv)>1 else 2000), t=float(sys.argv[2]) if len(sys.argv)>2 else 4.0):
    if l.startswith(('wrx','rx d')): print(l)
s.close()
