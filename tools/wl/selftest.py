#!/usr/bin/env python3
"""Harness self-check: both board ports are handed over (NDF_PORT = esp-ndf board, PEER_PORT = IDF peer) and exist."""
import os, sys
for k in ('NDF_PORT', 'PEER_PORT'):
    p = os.environ.get(k)
    if not p or not os.path.exists(p):
        print(f'FAIL {k} missing: {p}'); sys.exit(1)
print('ok both ports present')
