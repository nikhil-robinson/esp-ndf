#!/usr/bin/env python3
"""Receive block ack: the IDF AP (A-MPDU TX on) asks our station for a block-ack agreement (ADDBA request), we answer and program a hardware
session; the AP then aggregates, the MAC acknowledges the A-MPDUs by itself and a 256 KB TCP echo arrives intact.
Pass: addba >= 1, agg > 0 (frames received as part of an A-MPDU), echo bad 0 (the DEVKIT/BOX-3 is NDF_PORT, default the BOX-3)."""
import sys, time, re, os
sys.path.insert(0, os.path.dirname(__file__))
from ndf import Ndf
from peer import Peer
B = os.environ.get('NDF_PORT', '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:30:F9:5A:C1:10-if00')
import atexit; atexit.register(lambda: Ndf(port=B, ready='ip_unit ready').close())   # opening the port resets the board: never leave it a joined station
p = Peer(); p.reboot(); time.sleep(3); p.cmd('wifi_stop'); p.cmd('wifi_ap T - 6 -'); time.sleep(1); p.cmd('tcp_echo 8')
n = Ndf(port=B, ready='ip_unit ready'); assert 'rc 0' in n.cmd('up T', 40)[-1]
r = n.cmd('tcp 192.168.4.1 8 262144', 120)[-1]; m = n.cmd('mac')[-1]; print(r, m)
addba, agg = (int(re.search(k + r' (\d+)', m).group(1)) for k in ('addba', 'agg'))
ok = 'bad 0' in r and 'sent 262144' in r and 'rcvd 262144' in r and addba >= 1 and agg > 0
print('test_ba:', 'PASS' if ok else 'FAIL'); sys.exit(not ok)
