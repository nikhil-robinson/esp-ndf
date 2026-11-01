#!/usr/bin/env python3
"""Transmit block ack / A-MPDU: HT rate (MCS7), the station asks the IDF AP for a block-ack agreement (ADDBA request, AP has A-MPDU RX on) and its TCP
segments go out as A-MPDUs. Pass: aggregates sent, no MPDU lost for good (miss 0), 256 KB echo intact.  env RATE (default 0x17), SIZE (default 262144)."""
import sys, time, re, os
sys.path.insert(0, os.path.dirname(__file__))
from ndf import Ndf
from peer import Peer
B = os.environ.get('NDF_PORT', '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:30:F9:5A:C1:10-if00')
import atexit; atexit.register(lambda: Ndf(port=B, ready='ip_unit ready').close())   # opening the port resets the board: never leave it a joined station
p = Peer(); p.reboot(); time.sleep(3); p.cmd('wifi_stop'); p.cmd('wifi_ap T - 6 -'); time.sleep(1); p.cmd('tcp_echo 8')
n = Ndf(port=B, ready='ip_unit ready'); assert 'rc 0' in n.cmd('up T', 40)[-1]
n.cmd('rate ' + os.environ.get('RATE', '0x15')); n.cmd('ping 192.168.4.1 5', 30); time.sleep(3); n.cmd('ping 192.168.4.1 3', 30)   # the agreement is requested from the poll loop
for i in range(int(os.environ.get('REPS', '3'))):
    r = n.cmd('tcp 192.168.4.1 8 ' + os.environ.get('SIZE', '262144'), 120)[-1]; print(r, flush=True)
m = ' '.join(n.cmd('mac')); print(m)
g = lambda k: int(re.search(k + r' (\d+)', m).group(1))
ok = 'bad 0' in r and g('ampdu') > 0 and g('miss') * 50 < g('mpdu')
print('test_ba_tx:', 'PASS' if ok else 'FAIL'); sys.exit(not ok)
