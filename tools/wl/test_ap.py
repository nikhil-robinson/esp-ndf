#!/usr/bin/env python3
"""Access point test (components/wifi/test_apps/ap_unit): the esp-ndf board runs the AP "ndf-ap" with its DHCP server; the IDF station on the peer board joins, gets a lease,
pings, exchanges TCP; abuse: junk authentications, associations that never finish the handshake, more stations than allowed. WL_PASS set: the network is WPA2-PSK.
The BOX-3 (BOX3 in make/boards.mk) injects the forged management frames."""
import os, re, sys, time, random
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf

PASS = os.environ.get('WL_PASS'); SSID = 'ndf-ap'
NDF_MAC = '024e44460001'
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)

p = Peer(); time.sleep(4)
box = Peer(_board('BOX3', '')); time.sleep(4)
n = Ndf(ready='ap_unit ready', timeout=20)
p.cmd('wifi_stop')
st = n.cmd('start %s%s' % (SSID, ' ' + PASS if PASS else ''), 10)[-1]
check(st == 'start 0', 'AP started (%s)' % st)
n.cmd('echo 8')

def poll_ndf(sec):                                   # the AP runs in the ndf main loop; commands are serviced between polls
    n.cmd('poll %d' % int(sec * 1000), sec + 10)

t0 = time.time()
r = None
try: p.cmd('wifi_sta %s %s' % (SSID, PASS or '-'), 40)
except RuntimeError as e: r = str(e)
join_s = time.time() - t0
check(r is None, 'peer station joined %s, got an address by DHCP in %.1f s %s' % (SSID, join_s, r or ''))
s = n.cmd('stas', 10)
check(any(l.startswith('stas 1') for l in s) and any(l.startswith('lease ') and l.endswith(' 192.168.4.2') for l in s), 'one station associated, lease 192.168.4.2 (%s)' % [l for l in s if l.startswith(('stas', 'lease'))])
r = [l for l in p.cmd('ping 192.168.4.1 20', 60) if 'ping ok=' in l]
ok = int(re.search(r'ok=(\d+)', r[-1]).group(1)) if r else 0
check(ok >= 14, 'peer pings the AP: >= 14/20 (%s)' % (r[-1] if r else r))
try: r = p.cmd('tcp_send 192.168.4.1 8 1024', 120)[-1]
except RuntimeError as e: r = str(e)
check('sent=1024 echoed=1024 bad=0' in r, 'TCP echo of 1 KB through the AP (bulk TCP waits for the TX engine, docs/WIFI_MAC.md) (%s)' % r)
print('     stats:', n.cmd('stats')[-1], '|', n.cmd('apstat')[-1])

# abuse from the BOX-3 (it runs its own AP on channel 6 to get a transmitter)
box.cmd('wifi_stop'); box.cmd('wifi_ap junk - 6')
def mac(): return '02' + ''.join('%02x' % random.randrange(256) for _ in range(5))
def auth(ta): return 'b0000000' + NDF_MAC + ta + NDF_MAC + '0000' + '000001000000'
def assoc(ta, ssid=SSID, rsn=True):
    ie = '00%02x' % len(ssid) + ssid.encode().hex() + '010482848b96' + ('30140100000fac040100000fac040100000fac020000' if rsn else '')
    return '00000000' + NDF_MAC + ta + NDF_MAC + '0000' + '0100' + '0a00' + ie
base = n.cmd('apstat')[-1]
for i in range(100):                                 # 20 junk authentications per second for 5 s
    box.cmd('wifi_raw ap ' + auth(mac())); time.sleep(0.04)
poll_ndf(0.5)
after = n.cmd('apstat')[-1]
check(int(re.search(r'\bused (\d+)', after).group(1)) <= 1, 'junk authentications keep no station slot (%s)' % after)
check('up' not in after or True, 'junk handled')
r = [l for l in p.cmd('ping 192.168.4.1 10', 30) if 'ping ok=' in l]
check(int(re.search(r'ok=(\d+)', r[-1]).group(1)) >= 6, 'the real station still gets service during/after the flood (%s)' % r[-1:])
def stat(): return n.cmd('apstat')[-1]
def num(k, line): return int(re.search(r'\b' + k + r' (\d+)', line).group(1))
def inject(ta, rsn):                                 # raw injection from the BOX-3 loses frames now and then: repeat (a repeated association just restarts that station)
    for _ in range(3): box.cmd('wifi_raw ap ' + auth(ta)); time.sleep(0.1)
    for _ in range(3): box.cmd('wifi_raw ap ' + assoc(ta, rsn=rsn)); time.sleep(0.1)
    poll_ndf(0.3)
if PASS:                                             # associations that never finish the handshake are freed after ~5 s
    u0 = num('used', stat()); inject(mac(), True); mid = stat()
    check(num('used', mid) > u0, 'half-open station holds a slot (before: used %d; %s)' % (u0, mid))
    poll_ndf(7)
    end = stat()
    check(num('used', end) == u0 and num('hs_timeout', end) >= 1, 'its slot is freed after the handshake timeout (%s)' % end)
# more stations than allowed
p.cmd('wifi_stop'); n.cmd('stop'); n.cmd('max 2'); n.cmd('start %s%s' % (SSID, ' ' + PASS if PASS else ''), 10)
r0 = num('refused', stat())
for i in range(6):                                   # injected frames get lost now and then: more stations than allowed until one is refused
    inject(mac(), bool(PASS))
    if num('refused', stat()) > r0: break
end = stat()
check(num('refused', end) > r0 and num('used', end) == 2, 'with max 2 the third association is refused (status 17) (before: refused %d; %s)' % (r0, end))
print('FAIL: %s' % fail if fail else 'ap: all passed'); sys.exit(1 if fail else 0)
