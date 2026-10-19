#!/usr/bin/env python3
"""init_regs.json for gen_init.py from an idf_phydump PHY_ONLY log:
'pre' = pre_init, 'post' = phy_only, 'init' = words the vendor init changed and that then stay put
(phy_only == phy_only2: live counters and status are left out).
  python3 mkregs.py dump.txt > init_regs.json"""
import json, sys
st, cur = {}, None
for l in open(sys.argv[1], errors='replace'):
    l = l.strip()
    if l.startswith('@'): cur = st.setdefault(l[1:], {}); continue
    p = l.split()
    if cur is not None and len(p) == 2 and len(p[0]) == 8:
        try: cur[int(p[0], 16)] = int(p[1], 16)
        except ValueError: pass
pre, post, post2 = st['pre_init'], st['phy_only'], st['phy_only2']
init = sorted(a for a in post if pre.get(a) != post[a] and post2.get(a) == post[a] and 0x60005000 <= a < 0x60036000)
json.dump({'pre': {hex(a): v for a, v in pre.items()}, 'post': {hex(a): v for a, v in post.items()}, 'init': init}, sys.stdout)
