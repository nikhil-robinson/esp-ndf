import sys,time,re,json; sys.path.insert(0,'tools/wl')
from ndf import Ndf; from peer import Peer
A={int(k,16):int(v,16) for k,v in json.load(open('bench/wifi_regs/data/idf_box3_ht40_vs_ht20_txram.json'))['A'].items()}
p=Peer(); p.reboot(); time.sleep(3); p.cmd('wifi_stop'); p.cmd('wifi_ap T - 6'); time.sleep(1)
n=Ndf(port='/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:30:F9:5A:C1:10-if00', ready='ip_unit ready')
print(n.cmd('up T',40)[-1]); n.cmd('chan 6')
def cnt():
    a=int(re.search(r' rx (\d+)', n.cmd('mac')[-1]).group(1)); n.cmd('link 3000'); return int(re.search(r' rx (\d+)', n.cmd('mac')[-1]).group(1))-a
print('before: frames in 3 s', cnt())
ours={}
for lo,hi in ((0x60006000,0x60006200),(0x6001c000,0x6001f000)):
    for a in range(lo,hi,0x100):
        for l in n.cmd('rd 0x%x 64'%a,20):
            m=re.match(r'([0-9a-f]{8}) ([0-9a-f]{8})',l)
            if m: ours[int(m.group(1),16)]=int(m.group(2),16)
d=[a for a in sorted(ours) if a in A and A[a]!=ours[a] and not (0x60006014<=a<=0x60006030)]
print(len(d),'differ:', ' '.join('%x'%a for a in d))
for a in d: n.cmd('wr 0x%x 0x%x'%(a,A[a]))
print('after IDF HT40 FE/BB values: frames in 3 s', cnt())
