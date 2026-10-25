#!/usr/bin/env python3
"""Build every application (a folder whose Makefile includes make/project.mk); print failures and warnings."""
import os, subprocess, sys
root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
apps = []
for d, dirs, files in os.walk(root):
    dirs[:] = [x for x in dirs if x not in ('build', 'build-noaccel', '.git', 'bench')]
    if 'Makefile' in files and '$(NDF_PATH)/make/project.mk' in open(os.path.join(d, 'Makefile')).read():
        apps.append(d)
bad = 0
for d in sorted(apps):
    r = subprocess.run(['make', '-C', d, '-j8'], capture_output=True, text=True)
    out = r.stdout + r.stderr
    if r.returncode:
        bad += 1
        print('FAIL', os.path.relpath(d, root)); print('   ' + '\n   '.join([l for l in out.splitlines() if 'error' in l or 'overflow' in l][:3]))
    for l in out.splitlines():
        if 'warning:' in l: print('WARN', os.path.relpath(d, root), l.strip()[:150]); break
print(f'{len(apps)} applications, {bad} failed')
sys.exit(1 if bad else 0)
