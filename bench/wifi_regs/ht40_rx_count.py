import sys,time; sys.path.insert(0,'tools/wl')
from ndf import Ndf; from peer import Peer
p=Peer(); p.reboot(); time.sleep(3); p.cmd('wifi_stop')
n=Ndf(port='/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:30:F9:5A:C1:10-if00', ready='ip_unit ready')
p.cmd('wifi_ap T - 6'); time.sleep(1); print(n.cmd('up T',40)[-1]); print(n.cmd('chan 6')[-1]); n.cmd('link 2000'); print('beacon-window', n.cmd('mac')[-1])
for r in ('1m','6m'):
    print(r, [l for l in p.cmd('wifi_unicast 6 02:4e:44:46:00:02 50 %s 60 ap'%r, 60) if 'ack' in l][-1:], n.cmd('mac')[-1])
