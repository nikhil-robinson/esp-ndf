#!/usr/bin/env python3
"""The same as bench_ht_rates.py with an IDF station (bench/idf_wl_peer) on the BOX-3: 64 KB TCP echo per AP configuration "<chan> <rate|-> [20|below|above]"."""
import sys, time, re, os
sys.path.insert(0, 'tools/wl')
from peer import Peer, _board
p = Peer(); box = Peer(os.environ.get("IDF_STA") or _board("BOX3", ""))   # IDF_STA=<port> for another board
for ap in sys.argv[1:]:
    p.reboot(); box.reboot(); time.sleep(3); p.cmd('wifi_stop'); p.cmd('wifi_ap T - ' + ap); time.sleep(1); p.cmd('tcp_echo 8')
    box.cmd('wifi_stop')
    try: box.cmd('wifi_sta T -', 40)
    except Exception as e: print('IDF AP "%s" join fail' % ap, flush=True); continue
    tt = []
    for _ in range(5):
        try: r = box.cmd('tcp_send 192.168.4.1 8 65536', 90)[-1]; tt.append(int(re.search(r'elapsed_ms=(\d+)', r).group(1)) if 'bad=0' in r else -1)
        except Exception as e: tt.append(-1)
    print('IDF AP "%s"' % ap, sorted(tt), flush=True)
box.cmd('wifi_stop')
