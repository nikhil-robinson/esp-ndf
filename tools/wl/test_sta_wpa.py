#!/usr/bin/env python3
"""WPA2-PSK station test (components/wifi/test_apps/sta_unit): the IDF peer is a WPA2-PSK access point. The esp-ndf station joins with the 4-way
handshake, exchanges ARP through CCMP, fails fast with a wrong passphrase, and joins again after the AP restarts. IP traffic through CCMP:
WL_PASS=secret123 tools/ndf_test.py run components/lwip/test_apps/ip_unit"""
import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, ap_bssid
from ndf import Ndf

p = Peer(); p.reboot()
n = Ndf(ready='sta_unit ready', timeout=20)
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)

p.cmd('wifi_stop'); BSSID = ap_bssid(p.cmd('wifi_ap T secret123 6 0')); time.sleep(1); p.s.reset_input_buffer()
n.cmd('init')
sc = [l for l in n.cmd('scan', 30) if ' ssid T' in l]
check(len(sc) == 1 and ' sec 2 ' in sc[0], 'scan reports T as WPA2 (%s)' % sc[:1])
j = n.cmd('join T secret123', 20)[-1].split()
check(j[1] == '0', 'join with the 4-way handshake: %s' % ' '.join(j))
check('event ap-join 02:4e:44:46:00:02' in p.s.read(4096).decode(errors='replace'), 'the peer reports the association')
for tries in range(1, 4):
    a = n.cmd('arp 192.168.4.1', 10)[-1]
    if 'reply 1' in a: break
check('reply 1 from ' + BSSID in a, 'ARP request and reply through CCMP in %d tries (%s)' % (tries, a))
j = n.cmd('join T wrongpass1', 20)[-1].split()
check(j[1] == '-3' and int(j[3]) < 3500, 'wrong passphrase: handshake failure (-3) in < 3.5 s: %s' % ' '.join(j))
j = n.cmd('join T', 20)[-1].split()
check(j[1] == '-2', 'open join to a WPA2 network is refused (-2): %s' % ' '.join(j))
p.cmd('wifi_stop'); p.cmd('wifi_ap T secret123 6 0'); time.sleep(1)
j = n.cmd('join T secret123', 20)[-1].split()
check(j[1] == '0', 'join again after the AP restarted: %s' % ' '.join(j))
# forged EAPOL-Key frames from the "AP" (raw injection with the AP's address): wrong MIC, never accepted; the link and the keys stay as they are
def eapol(info, replay, nonce_byte, mic_byte, dl=0):
    body = bytes([2]) + info.to_bytes(2, 'big') + (16).to_bytes(2, 'big') + replay.to_bytes(8, 'big') + bytes([nonce_byte]) * 32 + bytes(16) + bytes(8) + bytes(8) + bytes([mic_byte]) * 16 + dl.to_bytes(2, 'big') + bytes(dl)
    return bytes([2, 3]) + len(body).to_bytes(2, 'big') + body
def frame(eapol_bytes):
    hdr = bytes.fromhex('0802' '0000' '024e44460002') + bytes.fromhex(BSSID.replace(':', '')) * 2 + bytes(2)
    return (hdr + bytes.fromhex('aaaa03000000888e') + eapol_bytes).hex()
st = n.cmd('status', 10)[-1]
check('up 1' in st, 'joined before the forgeries (%s)' % st)
for label, e in (('message 3, wrong MIC, new nonce', eapol(0x13ca, 0xFFFFFF, 0x55, 0xAA)), ('message 3, wrong MIC, replay counter far ahead', eapol(0x13ca, 1 << 40, 0x11, 0x22)),
                 ('group key message, wrong MIC', eapol(0x1382, 0xFFFFFF + 1, 0x00, 0x33, 40)), ('message 1 while connected', eapol(0x008a, 5, 0x77, 0))):
    p.cmd('wifi_raw ap ' + frame(e)); time.sleep(0.4)
    check('up 1' in n.cmd('poll 300', 10)[-1], 'forged %s: link stays up' % label)
for tries in range(1, 4):
    a = n.cmd('arp 192.168.4.1', 10)[-1]
    if 'reply 1' in a: break
check('reply 1 from ' + BSSID in a, 'traffic through CCMP still works after the forgeries (%s)' % a)
print('FAIL: %s' % fail if fail else 'sta_wpa: all passed'); sys.exit(1 if fail else 0)
