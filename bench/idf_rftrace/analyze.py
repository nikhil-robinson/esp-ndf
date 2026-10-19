#!/usr/bin/env python3
"""Turn an idf_rftrace log into the analog-bus model.

  python3 analyze.py trace.txt table_named.txt [--seq]

Replays every analog-bus write (rom_i2c_writeReg, rom_i2c_writeReg_Mask) in
order into a model of the analog register file (block, reg) -> byte, and for
the init and each marked phase prints which registers changed, the pbus writes
and the high-level ROM steps called. --seq also prints the init write sequence.
"""
import sys
from collections import OrderedDict

PH = {0x1: 'init (power on, PHY init, full calibration)', 0x2: 'set_chanfreq(f, 0)', 0x3: 'set_rf_freq_offset(0, f, 0)',
      0x4: 'set_rf_freq_offset(0, 2437, +125 kHz)', 0x5: 'force_rx_gain(1, g, 0)', 0x6: 'force_rx_gain(0, 0, 0) (AGC back)',
      0x7: 'set_rx_gain_table(2437, 0)', 0x8: 'tone on (txcal_debuge_mode, start_tx_tone_step)',
      0x9: 'tone off (start_tx_tone_step(0), txcal_work_mode)', 0xa: 'DAC TX on (stop_tx_tone, force_tx_gain(10,2), force_txon_mode(1))',
      0xb: 'force_txon_mode(0)', 0x63: 'end'}
I2C_W, I2C_WM, I2C_R, I2C_RM = 0x190 // 4, 0x198 // 4, 0x188 // 4, 0x194 // 4
PBUS_W = 0x1a8 // 4
STEPS_SKIP = {I2C_W, I2C_WM, I2C_R, I2C_RM, 0x16c // 4, 0x18c // 4, 0x154 // 4, 0x158 // 4, 0x15c // 4, 0x160 // 4, 0x164 // 4}


def main():
    trace, table = sys.argv[1], sys.argv[2]
    seq = '--seq' in sys.argv
    names = {}
    for l in open(table):
        p = l.split()
        names[int(p[0], 16) // 4] = p[2]
    recs = []
    for l in open(trace, errors='replace'):
        p = l.split()
        if len(p) == 10 and len(p[0]) == 4:
            try:
                recs.append((int(p[0], 16), int(p[1], 16), int(p[2]), int(p[3], 16), [int(x, 16) for x in p[4:]]))
            except ValueError:
                pass
    regs = {}                 # (block, reg) -> value (model)
    phase, pdesc = None, ''
    changes = OrderedDict()   # (block, reg) -> (old, new) within a phase
    steps = []
    pbus = []
    initseq = []

    def flush():
        if phase is None:
            return
        print('\n=== phase %s: %s' % (pdesc, PH.get(phase, '?')))
        if steps:
            print('  steps: ' + ', '.join(steps[:60]) + (' ...(%d)' % len(steps) if len(steps) > 60 else ''))
        if pbus:
            print('  pbus (row, col, value): ' + ' '.join('(%d,%d,0x%x)' % x for x in pbus[:40]))
        if changes:
            print('  analog registers changed (block.reg: old -> new):')
            line = []
            for (b, r), (o, n) in changes.items():
                line.append('%02x.%02x:%s->%02x' % (b, r, '--' if o is None else '%02x' % o, n))
            for i in range(0, len(line), 8):
                print('    ' + '  '.join(line[i:i + 8]))

    for idx, depth, done, ret, a in recs:
        if idx == 0xFFFF:
            flush()
            phase, pdesc = a[0], ('0x%x %d %d' % (a[0], a[1], a[2]))
            changes.clear(); steps.clear(); pbus.clear()
            continue
        if idx in (I2C_W, I2C_WM):
            b, r = a[0] & 0xff, a[2] & 0xff
            old = regs.get((b, r))
            if idx == I2C_W:
                new = a[3] & 0xff
            else:
                msb, lsb, d = a[3] & 0xff, a[4] & 0xff, a[5] & 0xff
                m = ((1 << (msb - lsb + 1)) - 1) << lsb
                new = (((old or 0) & ~m) | ((d << lsb) & m)) & 0xff
            if old != new:
                prev = changes.get((b, r), (old, None))[0]
                changes[(b, r)] = (prev, new)
            regs[(b, r)] = new
            if phase == 1:
                initseq.append('%02x.%02x=%02x' % (b, r, new) + ('' if idx == I2C_W else '[%d:%d]' % (a[3], a[4])))
        elif idx in (I2C_R, I2C_RM):
            b, r = a[0] & 0xff, a[2] & 0xff
            if idx == I2C_R and done and (b, r) not in regs:
                regs[(b, r)] = ret & 0xff
        elif idx == PBUS_W:
            pbus.append((a[0] & 0xff, a[1] & 0xff, a[2] & 0xffff))
        elif idx not in STEPS_SKIP and depth <= 2:
            n = names.get(idx, hex(idx * 4)).replace('rom_', '').replace('app:', '')
            args = ','.join('%x' % x for x in a[:3])
            steps.append('%s(%s)%s' % (n, args, '=%x' % ret if done and ret else ''))
    flush()
    print('\n=== analog register file after the run (%d registers):' % len(regs))
    blocks = sorted(set(b for b, _ in regs))
    for b in blocks:
        rr = sorted(r for bb, r in regs if bb == b)
        print('  block %02x: ' % b + ' '.join('%02x=%02x' % (r, regs[(b, r)]) for r in rr))
    if seq:
        print('\n=== init write sequence (%d writes):' % len(initseq))
        for i in range(0, len(initseq), 10):
            print('  ' + ' '.join(initseq[i:i + 10]))


main()
