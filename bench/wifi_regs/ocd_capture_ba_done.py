#!/usr/bin/env python3
"""JTAG capture of the IDF station's MAC state when an A-MPDU exchange has completed (ROM hal_mac_tx_get_blockack, queue 2): events, queue-2 status / block-ack words, to learn
what a successful exchange leaves in the registers.  Same setup as ocd_capture_ampdu.py (BOX-3 = IDF peer image, PEER = IDF AP, CH env = channel).  usage: ocd_capture_ba_done.py [captures]"""
import sys, os, time, json
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'wl')); sys.path.insert(0, os.path.dirname(__file__))
from ocd import Ocd
from peer import Peer, _board
N = int(sys.argv[1]) if len(sys.argv) > 1 else 3
GBA = 0x400052b0 + 3                                          # hal_mac_tx_get_blockack after its entry (ROM)
ap = Peer(); box = Peer(_board('BOX3', '')); ap.reboot(); ap.cmd('wifi_stop'); ap.cmd('wifi_ap T - %s -' % os.environ.get('CH', '1')); time.sleep(1); ap.cmd('tcp_echo 8')
box.reboot(); box.cmd('wifi_stop'); box.cmd('wifi_sta T -', 40)
box.s.write(b'tcp_send 192.168.4.1 8 3000000\n'); time.sleep(12)
out = []
with Ocd('30:30:F9:5A:C1:10') as o:
    tries = 0
    while len(out) < N and tries < 80:
        tries += 1
        o.cmd('halt'); o.cmd('bp 0x%x 2 hw' % GBA); o.cmd('resume'); o.cmd('wait_halt 8000')
        if o.reg('pc') != GBA: o.cmd('rbp 0x%x' % GBA); o.cmd('resume'); continue
        q = o.reg('a2'); print('regs a2 %x a10 %x a3 %x' % (q, o.reg('a10'), o.reg('a3')), flush=True); o.cmd('rbp 0x%x' % GBA)
        if q > 3: q = o.reg('a10') & 0xff if o.reg('a10') < 4 else 2
        h = o.mdw(0x60034300 - 76 * q, 1)[0]
        d = {'q': q, 'htsig': h, 'ev': o.mdw(0x60033c3c, 1)[0], 'cb0': o.mdw(0x60033cb0, 1)[0], 'mac': o.mdw(0x60034200, 192), 'ctl': o.mdw(0x60033c00, 128)}
        print('hit q%d htsig %08x ev %08x cb0 %x status %08x ba %08x %08x %08x' % (q, h, d['ev'], d['cb0'], o.mdw(0x60034320 - 76 * q, 1)[0], o.mdw(0x60034334 - 76 * q, 1)[0], o.mdw(0x60034328 - 76 * q, 1)[0], o.mdw(0x60034324 - 76 * q, 1)[0]), flush=True)
        if (h >> 24) & 0x7f == 0x0f: out.append(d)
        o.cmd('resume')
    o.cmd('resume')
json.dump(out, open(os.path.join(os.path.dirname(__file__), 'data', 'idf_ampdu_done.json'), 'w'))
print('captured', len(out))
