#!/usr/bin/env python3
"""MQTT 3.1.1 client (components/net/mqtt.c) on http_unit against a minimal broker on the IDF peer's AP (mqtt_broker, test code), with the
BOX-3 as an IDF station running esp-mqtt: a message from the IDF client reaches our subscription (wildcard), our QoS 1 publish is
acknowledged, a retained message arrives on a later subscription with the retain flag, a wrong password is refused (code 5), the will is
published when our connection vanishes (to an IDF subscriber), and round trips through the broker are timed for ours and esp-mqtt."""
import os, re, sys, time, threading
sys.path.insert(0, os.path.dirname(__file__))
from peer import Peer, _board
from ndf import Ndf

U = 'mqtt://192.168.4.1'
fail = []
def check(c, msg):
    print(('ok   ' if c else 'FAIL ') + msg)
    if not c: fail.append(msg)
def field(line, k): m = re.search(r'\b' + k + r' (-?\d+)', line); return int(m.group(1)) if m else -99

p = Peer(); box = Peer(_board('BOX3', '')); p.reboot(); box.reboot(); time.sleep(3)
import atexit; atexit.register(lambda: box.cmd('wifi_stop'))   # never leave the BOX-3 a station: after an AP restart it keeps its old address and collides with the next suite's DHCP lease
p.cmd('wifi_stop'); p.cmd('wifi_ap T - 6'); time.sleep(1); p.cmd('mqtt_broker')
n = Ndf(ready='http_unit ready', timeout=20)
r = n.cmd('up T', 40)[-1]; check(r.startswith('up rc 0'), 'joined (%s)' % r)
box.cmd('wifi_stop'); box.cmd('wifi_sta T -', 40)

c = n.cmd('mqconn ' + U, 20)[-1]; check(field(c, 'rc') == 0, 'CONNECT accepted (%s)' % c)
check(n.cmd('mqsub t/+ 1', 10)[-1] == 'mqsub 1', 'SUBSCRIBE t/+ granted QoS 1')
box.cmd('mqtt_pub %s t/a hi-from-idf 1' % U, 30)
g = n.cmd('mqrecv 1500', 10)[-1]; check(field(g, 'got') == 1 and 'topic t/a msg hi-from-idf' in g, 'esp-mqtt publish reaches our wildcard subscription (%s)' % g)
q = n.cmd('mqpub x/y hello 1', 10)[-1]; check(q == 'mqpub rc 0 acked 1', 'our QoS 1 publish is acknowledged (%s)' % q)
n.cmd('mqpub r/t keep 2', 10); n.cmd('mqdisc')
c = n.cmd('mqconn ' + U, 20)[-1]; n.cmd('mqsub r/t 0', 10)
g = n.cmd('mqrecv 1000', 10)[-1]; check(field(g, 'got') == 1 and 'msg keep retain 1' in g, 'retained message delivered on a new subscription with the retain flag (%s)' % g)
n.cmd('mqdisc')
b = n.cmd('mqconn %s user wrong' % U, 20)[-1]; check(field(b, 'rc') == 5, 'wrong password: CONNACK code 5 (%s)' % b)
b = n.cmd('mqconn %s user secret' % U, 20)[-1]; check(field(b, 'rc') == 0, 'right password accepted (%s)' % b)
n.cmd('mqdisc')
check(n.cmd('mqwill %s w/t' % U, 20)[-1] == 'mqwill rc 0', 'CONNECT with a will')
import threading
res = {}
th = threading.Thread(target=lambda: res.setdefault('w', box.cmd('mqtt_sub %s w/t 5000' % U, 30)[-1])); th.start()
time.sleep(2.5); n.cmd('mqdrop'); th.join()
check(field(res.get('w', ''), 'got') == 1, 'our connection vanishes: the broker publishes the will to an IDF subscriber (%s)' % res.get('w'))

ours0 = n.cmd('mqbench %s 20 100' % U, 120)[-1]; ours1 = n.cmd('mqbench %s 1020 100' % U, 120)[-1]
idf0 = box.cmd('mqtt_bench %s 20 100 0' % U, 120)[-1]; idf1 = box.cmd('mqtt_bench %s 20 100 1' % U, 120)[-1]
check(field(ours0, 'echoes') == 20 and field(ours1, 'echoes') == 20, 'ours: 20 round trips at QoS 0 and QoS 1 (%s | %s)' % (ours0, ours1))
check(field(idf0, 'echoes') == 20 and field(idf1, 'echoes') == 20, 'esp-mqtt: 20 round trips at QoS 0 and QoS 1 (%s | %s)' % (idf0, idf1))
print('     ours QoS0: %s\n     ours QoS1: %s\n     IDF  QoS0: %s\n     IDF  QoS1: %s' % (ours0, ours1, idf0, idf1))
print('FAIL: %s' % fail if fail else 'mqtt: all passed'); sys.exit(1 if fail else 0)
