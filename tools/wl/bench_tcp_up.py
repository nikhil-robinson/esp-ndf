#!/usr/bin/env python3
"""TCP upload throughput to the IDF AP's iperf_rx sink: esp-ndf ip_unit `tcpup` on the BOX-3 (build with -DNDF_WIFI_AMPDU_TX=0 / 1) or, with IDF=1, the IDF station (iperf_tx) on the same board and air.
Prints per-rep kbit/s and the median.  env CH (default 1), RATE (default 0x17), BYTES (default 1048576), REPS (default 8), IDF=1.   usage: bench_tcp_up.py"""
import sys, os, re, time, statistics
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf
CH, BYTES, REPS = os.environ.get('CH', '1'), int(os.environ.get('BYTES', 1 << 20)), int(os.environ.get('REPS', 8))
ap = Peer(); ap.reboot(); time.sleep(2); ap.cmd('wifi_stop'); ap.cmd('wifi_ap T - %s -' % CH); time.sleep(1)
idf = os.environ.get('IDF') == '1'
if idf:
    n = Peer(_board('BOX3', '')); n.reboot(); n.cmd('wifi_stop'); print(n.cmd('wifi_sta T -', 40)[-1]); time.sleep(5)
else:
    n = Ndf(port=_board('BOX3', ''), ready='ip_unit ready'); assert 'rc 0' in n.cmd('up T', 40)[-1]
    n.cmd('rate ' + os.environ.get('RATE', '0x17')); n.cmd('ping 192.168.4.1 5', 30); time.sleep(4); n.cmd('ping 192.168.4.1 3', 30)
res = []
for i in range(REPS):
    ap.s.reset_input_buffer(); ap.s.write(b'iperf_rx 9 40\n'); time.sleep(1)
    if idf: r = n.cmd('iperf_tx 192.168.4.1 9 5', 30)[-1]; k = int(re.search(r'kbit_s=(\d+)', r).group(1))
    else:
        r = n.cmd('tcpup 192.168.4.1 9 %d' % BYTES, 60)[-1]; m = re.search(r'sent (\d+) .* in (\d+) ms', r); k = int(m.group(1)) * 8 // max(1, int(m.group(2)))
    time.sleep(1); print('rep %d: %d kbit/s  (%s)' % (i, k, r), flush=True); res.append(k)
    ap.s.read(4096)
print('median %d kbit/s  min %d max %d' % (statistics.median(res), min(res), max(res)))
if not idf: print(' '.join(n.cmd('mac')))
