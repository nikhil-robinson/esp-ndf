#!/usr/bin/env python3
"""Discover, flash, run and judge esp-ndf test applications (docs/TESTING.md).

  ndf_test.py list   [PATH...] [--kind unit|bench|manual|example|all]
  ndf_test.py run    [PATH...] [--kind unit] [--board devkit|box3] [--no-build] [--keep-going]
  ndf_test.py run    components/drivers/spi/test_apps/spi_unit        # one application
  ndf_test.py run    components/drivers                               # every test app below a folder

A test application is a folder with a Makefile that includes make/project.mk and sets, before the include:
  TEST_KIND    unit (self-checking, run by default) | bench (prints numbers) | manual (scope / two boards / host script)
  TEST_BOARD   devkit | box3 | any            board it needs (any = devkit)
  TEST_PASS    regex that ends the run and counts as a pass   (default: "=== unit: N/N passed ===" with N equal)
  TEST_TIMEOUT seconds to wait for it                          (default 60)
  TEST_HOST    host script to run after flashing (manual tests)
  TEST_NOTE    wiring / setup the test needs
  TEST_ALLOW   words that are expected in the console and must not fail the run (crash tests print PANIC)
  TEST_QUIET   seconds to keep watching after the pass line: a second pass line (a reboot loop) fails the run
A run fails when TEST_PASS is not seen in time or the console shows a line with FAIL / PANIC.
"""
import argparse, os, re, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def boards():
    b = {}
    for line in open(os.path.join(ROOT, 'make', 'boards.mk')):
        m = re.match(r'(\w+)\s*=\s*(\S+)', line)
        if m: b[m.group(1).lower()] = m.group(2)
    return b


def discover(paths):
    apps = []
    for p in paths or ['.']:
        p = os.path.abspath(p)
        for d, dirs, files in os.walk(p):
            dirs[:] = [x for x in dirs if x not in ('build', 'build-noaccel', '.git', 'bench', 'managed_components')]
            if 'Makefile' in files and '$(NDF_PATH)/make/project.mk' in open(os.path.join(d, 'Makefile')).read():
                apps.append(d)
    return sorted(apps)


def info(app):
    out = subprocess.run(['make', '-s', '-C', app, 'ndf-info'], capture_output=True, text=True).stdout
    kv = dict(l.split('=', 1) for l in out.splitlines() if '=' in l)
    kv['TEST_TIMEOUT'] = int(kv.get('TEST_TIMEOUT') or 60); kv['TEST_QUIET'] = int(kv.get('TEST_QUIET') or 0)
    return kv


def port_ready(port, grace=20):
    """the port of a board that just reset on purpose comes back within a few seconds; a board that does not is left alone"""
    t = time.time()
    while not os.path.exists(port):
        if time.time() - t > grace: return False
        time.sleep(0.5)
    return True


MAKE_ARGS = []


def run_one(app, kv, port, build=True):
    name = os.path.relpath(app, ROOT)
    if not port_ready(port):
        return 'NOPORT', f'{port} missing'
    if build:
        r = subprocess.run(['make', '-C', app, '-j8', 'flash', f'PORT={port}'] + MAKE_ARGS, capture_output=True, text=True, timeout=300)
        if r.returncode:
            return 'BUILD', (r.stdout + r.stderr)[-600:]
    if kv.get('TEST_HOST'):                                 # two-board / host-driven test: the script judges by its exit code
        env = dict(os.environ, NDF_PORT=port, PEER_PORT=boards().get('peer', boards()['box3']))
        r = subprocess.run([sys.executable, os.path.join(ROOT, kv['TEST_HOST'])], env=env, capture_output=True, text=True,
                           timeout=kv['TEST_TIMEOUT'])
        out = (r.stdout + r.stderr).strip().splitlines()
        if r.returncode:
            os.makedirs(os.path.join(ROOT, 'build', 'testlogs'), exist_ok=True)
            open(os.path.join(ROOT, 'build', 'testlogs', name.replace('/', '__') + '.log'), 'w').write('\n'.join(out) + '\n')
        return ('PASS' if not r.returncode else 'FAIL'), (out[-1] if out else '')
    pat = re.compile(kv['TEST_PASS'])
    p = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'run_app.py'), port, '@@never@@', str(kv['TEST_TIMEOUT'])],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log, verdict = [], None
    allow = kv.get('TEST_ALLOW', '').split()
    t_pass = None
    for line in p.stdout:
        line = line.rstrip()
        log.append(line)
        bad = re.search(r'\bFAIL\b|PANIC', line)
        if bad and bad.group(0) not in allow:
            verdict = verdict or ('FAIL', line.strip())
        m = pat.search(line)
        if m:
            if t_pass is not None:                       # the pass line again during the quiet time: it rebooted
                verdict = ('FAIL', 'pass line seen twice (reboot loop?): ' + line.strip()); break
            ok = True
            if len(m.groups()) >= 2 and m.group(1).isdigit():         # "P/N passed": every test passed
                ok = m.group(1) == m.group(2)
            if not (verdict and verdict[0] == 'FAIL'):
                verdict = ('PASS', line.strip()) if ok else ('FAIL', line.strip())
            if not kv['TEST_QUIET']: break
            t_pass = time.time()
        if t_pass is not None and time.time() - t_pass > kv['TEST_QUIET']: break
    p.kill()
    if verdict is None:
        verdict = ('TIMEOUT', f'no "{kv["TEST_PASS"]}" in {kv["TEST_TIMEOUT"]} s')
    if verdict[0] != 'PASS':
        os.makedirs(os.path.join(ROOT, 'build', 'testlogs'), exist_ok=True)
        open(os.path.join(ROOT, 'build', 'testlogs', name.replace('/', '__') + '.log'), 'w').write('\n'.join(log) + '\n')
    return verdict[0], verdict[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['list', 'run'])
    ap.add_argument('paths', nargs='*')
    ap.add_argument('--kind', default='unit')
    ap.add_argument('--board', default=None, help='run only tests that need this board')
    ap.add_argument('--no-build', action='store_true', help='do not build / flash, run what is on the board')
    ap.add_argument('--port', default=None, help='serial port of the board (overrides the board table), e.g. the ESP32-P4')
    ap.add_argument('--make-args', default='', help='extra make arguments, e.g. "SOC=esp32p4 P4_REV=1 XIP=off"')
    ap.add_argument('--keep-going', action='store_true', default=True)
    a = ap.parse_args()
    MAKE_ARGS[:] = a.make_args.split()
    b = boards()
    if a.port:
        b = {k: a.port for k in b}
    apps = discover(a.paths)
    rows = []
    for d in apps:
        kv = info(d)
        if a.kind != 'all' and kv['TEST_KIND'] != a.kind: continue
        board = 'devkit' if kv['TEST_BOARD'] == 'any' else kv['TEST_BOARD']
        if a.board and board != a.board: continue
        rows.append((d, kv, board))
    if a.cmd == 'list':
        for d, kv, board in rows:
            print(f'{kv["TEST_KIND"]:8} {board:7} {os.path.relpath(d, ROOT)}' + (f'   # {kv["TEST_NOTE"]}' if kv['TEST_NOTE'] else ''))
        print(f'{len(rows)} applications')
        return 0
    res = []
    for d, kv, board in rows:
        name = os.path.relpath(d, ROOT)
        print(f'--- {name} ({board})', flush=True)
        v, msg = run_one(d, kv, b[board], not a.no_build)
        print(f'    {v}: {msg}', flush=True)
        res.append((name, v, msg))
    bad = [r for r in res if r[1] != 'PASS']
    print(f'\n{len(res) - len(bad)}/{len(res)} passed')
    for n, v, m in bad:
        print(f'  {v:8} {n}: {m}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
