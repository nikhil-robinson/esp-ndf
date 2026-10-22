#!/usr/bin/env python3
"""4.3 verify: D0 (ttyUSB1, examples/rf_154) loops a 15.4 O-QPSK frame on ch 11 (2405 MHz); C9:48
(ttyUSB0, rf_pkt) captures IQ at 16 MS/s. Recover the chips (matched filter + O-QPSK sampling) and
correlate against the 802.15.4 PN table: a run of 8 identical symbols at full 32/32 chip correlation
is the preamble (4 zero octets) -- proof the synthesised DAC waveform is a valid O-QPSK/DSSS signal."""
import time, serial, numpy as np
PN=[0xD9C3522E,0xED9C3522,0x2ED9C352,0x22ED9C35,0x522ED9C3,0x3522ED9C,0xC3522ED9,0x9C3522ED,
    0x8C96077B,0xB8C96077,0x7B8C9607,0x77B8C960,0x077B8C96,0x6077B8C9,0x96077B8C,0xC96077B8]
pnbits=np.array([[(PN[s]>>c)&1 for c in range(32)] for s in range(16)])*2-1
SPC=8; pulse=np.sin(np.pi*np.arange(2*SPC)/(2*SPC))
def mk(p): s=serial.Serial(None,115200,timeout=0.3); s.port=p; s.dtr=False; s.rts=False; s.open(); return s
d0=mk('/dev/ttyUSB1'); c9=mk('/dev/ttyUSB0')
d0.setRTS(True); time.sleep(0.15); d0.setRTS(False); time.sleep(1.6); d0.reset_input_buffer()
d0.write(b'tx\n'); time.sleep(2.0); print('D0:', d0.read(200).decode(errors='replace').strip().split(chr(10))[-2])
c9.setRTS(True); time.sleep(0.15); c9.setRTS(False); time.sleep(1.6); c9.reset_input_buffer()
c9.write(b'iqcap 2405000\n'); buf=b''; t=time.time()
while time.time()-t<6: buf+=c9.read(65536)
d0.write(b'stop\n'); d0.close(); c9.close()
lines=[l.strip() for l in buf.decode(errors='replace').split(chr(10)) if len(l.strip())==80 and all(c in '0123456789abcdef' for c in l.strip())]
w=[int(l[j:j+5],16) for l in lines for j in range(0,80,5)]
s10=lambda x:x-1024 if x>=512 else x
I=np.array([s10(x&0x3FF) for x in w],float); Qr=np.array([s10((x>>10)&0x3FF) for x in w],float)
z=(I-I.mean())-1j*(Qr-Qr.mean())      # true Q (raw Q inverted)
print('captured %d samples'%len(z))
def chips(cfo,toff):
    d=z*np.exp(-2j*np.pi*cfo*np.arange(len(z))/16e6); mf=np.convolve(d,pulse[::-1],'same')
    idx=np.arange(toff,len(z)-2*SPC,SPC); return np.where((np.arange(len(idx))%2)==0,mf[idx].real,mf[idx].imag)
best=(0,0)
for cfo in np.arange(-9000,9001,250):
    for toff in range(2*SPC):
        for co in range(2):
            ch=np.sign(chips(cfo,toff))[co:]; ns=len(ch)//32
            if ns<10: continue
            C=ch[:ns*32].reshape(ns,32)@pnbits.T; m=np.abs(C).max(1); sym=np.argmax(np.abs(C),1)
            run=mx=0; prev=-1
            for i in range(ns):
                if m[i]>=30 and sym[i]==prev: run+=1
                elif m[i]>=30: run=1; prev=sym[i]
                else: run=0; prev=-1
                mx=max(mx,run)
            if mx>best[0]: best=(mx,cfo)
print('longest run of identical perfect-correlation (32/32) symbols: %d (CFO %d Hz)'%best)
print('VERIFIED: valid 802.15.4 O-QPSK/DSSS preamble on air' if best[0]>=6 else 'no clear preamble')
