#!/usr/bin/env python3
"""64 KB TCP echo of esp-ndf ip_unit on the BOX-3 against the IDF AP in the given configurations, each "<chan> <rate|-> [20|below|above]"
(wifi_ap T - <args>), e.g. "6 -" "6 0x17" "6 0x17 20" "9 0x17 below". Prints the echo times and how many received frames were 40 MHz wide.
Data: bench/idf_wl_peer/data/ht40_*.txt"""
import sys, time, re
sys.path.insert(0, 'tools/wl')
from ndf import Ndf
from peer import Peer
import os
B = os.environ.get('NDF_PORT', '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:30:F9:5A:C1:10-if00')   # the BOX-3; NDF_PORT for another board
import atexit; atexit.register(lambda: Ndf(port=B, ready='ip_unit ready').close())   # opening the port resets the board: never leave it a joined station (its traffic skewed later runs)
for ap in sys.argv[1:]:
    p = Peer(); p.reboot(); time.sleep(3); p.cmd('wifi_stop'); p.cmd('wifi_ap T - ' + ap); time.sleep(1); p.cmd('tcp_echo 8')
    n = Ndf(port=B, ready='ip_unit ready'); u = n.cmd('up T', 40)[-1]
    if 'rc 0' not in u: print('AP "%s"' % ap, u, flush=True); n.close(); continue
    tt = []
    for _ in range(5):
        r = n.cmd('tcp 192.168.4.1 8 65536', 90)[-1]; m = re.search(r'in (\d+) ms', r); tt.append(int(m.group(1)) if m and 'bad 0' in r else -1)
    print('AP "%s"' % ap, sorted(tt), 'cwb40', re.search(r'cwb40 (\d+)', n.cmd('mac')[-1]).group(1), flush=True); n.close()
