#!/usr/bin/env python3
"""JTAG capture of the IDF station's A-MPDU transmit (ESP-IDF 5.3.5 peer image on the BOX-3, AP = IDF peer on C9:48 with A-MPDU on): a TCP upload makes the
station aggregate. One halt per stage and no halts on the frames in between (a halt longer than the ACK timeout makes the driver retry the same frame, which
halts again): ppAssembleAMPDU -> the next mac_tx_set_txop_q (kept when its TX info says several MPDUs) -> hal_mac_txq_enable. Dumps the TX info list, the hardware
TX headers, the descriptors, the hwtxop list and the MAC registers to bench/wifi_regs/data/idf_ampdu_tx_<n>.json.  usage: ocd_capture_ampdu.py [captures]"""
import sys, os, time, json
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'wl')); sys.path.insert(0, os.path.dirname(__file__))
from ocd import Ocd
from peer import Peer, _board
N = int(sys.argv[1]) if len(sys.argv) > 1 else 1
AMP, TXOP, TXEN = 0x420d1f1c + 3, 0x420a7fb8 + 3, 0x4038a734   # ppAssembleAMPDU, mac_tx_set_txop_q (after their entry), hal_mac_txq_enable: nm of bench/idf_wl_peer/build/idf_wl_peer.elf
RG = ((0x60033c00, 0x60033e00), (0x60034200, 0x60034500))
ap = Peer(); box = Peer(_board('BOX3', '')); ap.reboot(); ap.cmd('wifi_stop'); ap.cmd('wifi_ap T - %s -' % os.environ.get('CH', '6')); time.sleep(1); ap.cmd('tcp_echo 8')
def halted(o, bp, ms=8000):
    # bp set with the CPU halted, then run until it is hit
    o.cmd('halt'); o.cmd('bp 0x%x 2 hw' % bp); o.cmd('resume'); o.cmd('wait_halt %d' % ms); ok = o.reg('pc') == bp; o.cmd('rbp 0x%x' % bp); return ok
def dump_txinfo(o, p):
    d = {'addr': p, 'w': o.mdw(p, 32)}
    h = d['w'][11] if len(d['w']) > 11 else 0
    if 0x3fc00000 <= h < 0x3fd00000: d['hdr'] = o.mdw(h, 40)
    return d
box.reboot(); box.cmd('wifi_stop'); box.cmd('wifi_sta T -', 40)
box.s.write(b'tcp_send 192.168.4.1 8 3000000\n'); time.sleep(12)   # the station asks for the block-ack agreement after some seconds of traffic
with Ocd('30:30:F9:5A:C1:10') as o:
    n = tries = 0
    while n < N and tries < int(os.environ.get('TRIES', 60)):
        tries += 1
        if not halted(o, AMP): print('no assemble', flush=True); o.cmd('resume'); continue
        if not halted(o, TXEN): print('no txq_enable', flush=True); o.cmd('resume'); continue
        qi = o.reg('a2'); ht = o.mdw(0x60034300 - 76 * qi, 1)[0]
        if ht >> 24 != 0x0f: print('queue', qi, 'htsig %08x' % ht, flush=True); o.cmd('resume'); continue          # not an aggregate (HT-SIG aggregation bit)
        d = {'regs': {k: o.reg(k) for k in ('pc', 'a2', 'a3')}, 'mac': {'%08x' % lo: o.mdw(lo, (hi - lo) // 4) for lo, hi in RG}}
        q = o.mdw(0x60033d08 - 8 * qi, 1)[0]; dp = 0x3fc00000 | (q & 0xFFFFF); d['start'] = q; d['desc'] = []; seen = set()      # queue 2 start word = 0x60033d08 - 2 * 8
        while dp not in seen and len(seen) < 16:
            seen.add(dp); ds = o.mdw(dp, 4); buf = o.mdw(ds[1], 480) if ds and 0x3fc00000 <= ds[1] < 0x3fd00000 else []
            d['desc'].append({'addr': dp, 'words': ds, 'buf': buf}); dp = ds[2] if ds else 0
        json.dump(d, open(os.path.join(os.path.dirname(__file__), 'data', 'idf_ampdu_tx_%d.json' % n), 'w'))
        print('capture', n, 'start %08x' % d['start'], 'desc', len(d['desc']), 'tries', tries, flush=True); n += 1
        o.cmd('resume')
    o.cmd('resume'); print('box:', box.s.read(2000))
