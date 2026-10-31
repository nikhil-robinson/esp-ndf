#!/usr/bin/env python3
"""Same-board register diff of an IDF station (BOX-3, bench/idf_wl_peer) joined to the IDF AP (PEER) in two configurations, each captured twice:
only registers stable within a configuration and different between them are reported. MMIO (FE, BB, MAC) and the regi2c blocks 0x61-0x6d.
usage: diff_idf_sta.py <out.json> <ap args A> <sta args A> <ap args B> <sta args B>   e.g. ht40_vs_ht20.json "T - 6" "" "T - 6 - 20" ""
(the n-vs-bg capture in data/idf_box3_n_vs_bg.json was  "T - 6" "" "T - 6" " bg")"""
import sys, time, re, json, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools/wl'))
from peer import Peer, _board
def mm(dev):
    out = {}
    for lo, hi in [(0x60005000, 0x60006200), (0x6001c000, 0x6001f000), (0x60033000, 0x60034400)]:   # 0x600342e0..3fc: TX parameter RAM
        for a in range(lo, hi, 0x100):
            for l in dev.cmd('rd 0x%x 64' % a, 20):
                m = re.match(r'\s*([0-9a-f]{8}) ([0-9a-f]{8})', l)
                if m: out[int(m.group(1), 16)] = int(m.group(2), 16)
    return out
def i2(dev): return {'%02x' % b: [x for x in dev.cmd('i2crd 0x%x' % b, 10) if x.strip().startswith('i2c')][-1].split()[2:] for b in range(0x61, 0x6e)}
out, apA, stA, apB, stB = sys.argv[1:6]
p = Peer(); box = Peer(_board('BOX3', '')); res = {}
for k, ap, st in [('A', apA, stA), ('B', apB, stB)] * 2:
    p.reboot(); box.reboot(); time.sleep(3); p.cmd('wifi_stop'); p.cmd('wifi_ap ' + ap); time.sleep(1); p.cmd('tcp_echo 8')
    box.cmd('wifi_stop'); box.cmd('wifi_sta T -' + st, 40); r = [l for l in box.cmd('tcp_send 192.168.4.1 8 65536', 120) if 'elapsed' in l][-1]
    res.setdefault(k, []).append((mm(box), i2(box))); print(k, r, flush=True)
box.cmd('wifi_stop')
a1, a2 = res['A']; b1, b2 = res['B']
stable = [a for a in a1[0] if a1[0][a] == a2[0][a] and b1[0][a] == b2[0][a] and a1[0][a] != b1[0][a]]
for a in sorted(stable): print('%08x A %08x B %08x' % (a, a1[0][a], b1[0][a]))
i2d = []
for b in a1[1]:
    for r in range(min(len(a1[1][b]), 32)):
        if a1[1][b][r] == a2[1][b][r] and b1[1][b][r] == b2[1][b][r] and a1[1][b][r] != b1[1][b][r]:
            print('i2c %s reg %d A %s B %s' % (b, r, a1[1][b][r], b1[1][b][r])); i2d.append([b, r, a1[1][b][r], b1[1][b][r]])
json.dump({'args': sys.argv[2:6], 'A': {hex(a): hex(a1[0][a]) for a in a1[0]}, 'B': {hex(a): hex(b1[0][a]) for a in b1[0]},
           'A_i2c': a1[1], 'B_i2c': b1[1], 'stable': [hex(a) for a in sorted(stable)], 'i2c_diff': i2d}, open(out, 'w'), indent=0)
