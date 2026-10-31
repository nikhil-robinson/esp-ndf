import sys, time, re
sys.path.insert(0,'tools/wl')
from ndf import Ndf
from peer import Peer
B='/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:30:F9:5A:C1:10-if00'
cap=sys.argv[1]; steps=sys.argv[2].split(',')
p=Peer(); p.reboot(); time.sleep(3); p.cmd('wifi_stop'); p.cmd('wifi_ap T - 6 0x14'); time.sleep(1); p.cmd('tcp_echo 8')
n=Ndf(port=B, ready='ip_unit ready'); print(n.cmd('up T',40)[-1])
def t():
    try: return [re.search(r'in (\d+) ms', n.cmd('tcp 192.168.4.1 8 65536',30)[-1]).group(1) for _ in range(2)]
    except Exception as e: return 'stall'
print('base', t(), flush=True)
for s in steps:
    if s=='lo': 
        for r,v in ((1,cap),(5,cap)): n.cmd('i2cwr 0x62 %d 0x%s'%(r,v))
        for r,v in ((3,'31'),(4,'91'),(5,'11')): n.cmd('i2cwr 0x63 %d 0x%s'%(r,v))
    if s=='fe': n.cmd('wr 0x600060c0 0xb0f78413')
    print('after', s, t(), flush=True)
