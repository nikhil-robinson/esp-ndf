#!/usr/bin/env python3
"""Host side of examples/rf/rfctl: drive one or two boards by line commands.
   b = Board(port); b.cmd('init'); b.cmd('meas 8192 2e6')  -> list of result lines"""
import serial, time

C948 = '/dev/serial/by-path/pci-0000:05:00.3-usb-0:2:1.0-port0'   # CH340 UART (both boards on CH340: by physical port)
D0 = '/dev/serial/by-path/pci-0000:05:00.4-usb-0:1:1.0-port0'   # C9:D0 (moved to port 1 on 2026-10-22)


class Board:
    def __init__(self, port, reset=True, timeout=20):
        self.s = serial.Serial(None, 115200, timeout=0.2)
        self.s.port = port; self.s.dtr = False; self.s.rts = False   # opening must not reset the board
        self.s.open()
        if reset:
            self.s.rts = True; time.sleep(0.1); self.s.rts = False
            self.wait('rfctl ready', timeout)

    def wait(self, token, timeout=20):
        buf, end = b'', time.time() + timeout
        while time.time() < end:
            buf += self.s.read(4096)
            if token.encode() in buf:
                return buf.decode(errors='replace')
        raise TimeoutError('no %r in %r' % (token, buf[-200:]))

    def cmd(self, line, timeout=30):
        self.s.write((line + '\n').encode())
        out, end, buf = [], time.time() + timeout, b''
        while time.time() < end:
            buf += self.s.read(4096)
            while b'\n' in buf:
                l, buf = buf.split(b'\n', 1)
                l = l.decode(errors='replace').strip()
                if l == 'ok':
                    return out
                if l.startswith('err'):
                    raise RuntimeError(l)
                if l:
                    out.append(l)
        raise TimeoutError(line)

    def meas(self, n, *freqs):
        """-> dict(dc=(i,q), rms, peak, bins=[dB...])"""
        l = [x for x in self.cmd('meas %d %s' % (n, ' '.join('%g' % f for f in freqs))) if x.startswith('meas')][0].split()
        return dict(rc=int(l[2]), dc=(float(l[4]), float(l[5])), rms=float(l[7]), peak=int(l[9]), bins=[float(x) for x in l[10:]])


def link(min_db=25, tries=4, log=print):
    """Both boards initialised and tuned, D0 sending a +2 MHz tone at 2437 MHz, C9:48 receiving at 2436 MHz,
    checked: the tone must exceed min_db at RX gain 20 (rare sessions come up deaf; re-initialise them)."""
    for k in range(tries):
        rx = Board(C948); tx = Board(D0)
        rx.cmd('init'); tx.cmd('init'); rx.cmd('rxcal')
        tx.cmd('tunetx 2437000'); rx.cmd('tune 2436000'); rx.cmd('gain 20')
        tx.cmd('tx 127 0 2000 300')
        t = rx.meas(8192, 3e6)['bins'][0]
        if t > min_db:
            return rx, tx
        log('link check failed (%.1f dB), re-initialising (attempt %d)' % (t, k + 1))
        rx.s.close(); tx.s.close()
    raise RuntimeError('no link')
