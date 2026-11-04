#!/usr/bin/env python3
"""10 characterization: received level vs TX power index. D0 (ttyUSB1) transmits continuously at each
index; C9:48 (ttyUSB0) measures RSSI (AGC, absolute dBm). Both run rf_pkt."""
import serial, time, re, threading
def mk(p): s=serial.Serial(None,115200,timeout=0.3); s.port=p; s.dtr=False; s.rts=False; s.open(); return s
d0=mk('/dev/ttyUSB1'); c9=mk('/dev/ttyUSB0')
for b in (d0,c9): b.setRTS(True); time.sleep(0.15); b.setRTS(False)
time.sleep(1.8); d0.reset_input_buffer(); c9.reset_input_buffer()
SYNC='0x71764129'; rows=[]
def rx_once(res):
    out=c9.readline  # placeholder
for p in (5,6,7,8,9,10,11,12,13,14,15):
    d0.write(b'stop\n'); time.sleep(0.2); d0.reset_input_buffer()
    d0.write(('txp %d\n'%p).encode()); time.sleep(0.15); d0.read(50)
    res={}
    def rxjob():
        c9.reset_input_buffer(); c9.write(('rx 2440 0 %s 0 -1 600\n'%SYNC).encode())
        t=time.time()
        while time.time()-t<3:
            l=c9.readline().decode(errors='replace').strip()
            if l.startswith('rx rc 0 good'): res['l']=l; return
    th=threading.Thread(target=rxjob); th.start(); time.sleep(0.25)
    d0.write(('send 2440 0 %s 0 32 0\n'%SYNC).encode())
    th.join(); d0.read(200)
    m=re.search(r'good (\d+).*rssi (-?\d+)', res.get('l',''))
    rows.append((p, m.group(2) if m else '?', m.group(1) if m else '?'))
    print('idx %2d  rssi %4s dBm  good %s'%(rows[-1]))
d0.write(b'stop\n'); d0.close(); c9.close()
open('bench/rfmeas/level_vs_index_10.txt','w').write('# 10: received RSSI (AGC, dBm) vs TX power index, 2440 MHz, 1M, two boards\n# idx rssi_dBm good\n'+'\n'.join('%s %s %s'%r for r in rows)+'\n')
