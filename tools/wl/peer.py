"""Line-command client for the ESP-IDF peer (bench/idf_wl_peer) on the BOX-3.
The peer answers every command with output lines and then "ok" or "err <why>"."""
import os, re, sys, time, serial

def _board(name, default):                        # make/boards.mk is the one place that maps boards to USB ids
    try:
        for l in open(os.path.join(os.path.dirname(__file__), '..', '..', 'make', 'boards.mk')):
            m = re.match(r'%s\s*=\s*(\S+)' % name, l)
            if m: return m.group(1)
    except OSError: pass
    return default

BOX3 = _board('PEER', _board('BOX3', ''))         # the IDF peer: PEER if set, else the BOX-3


def ap_bssid(lines):
    """the BSSID the peer's wifi_ap printed (\"ap T ch 6 bssid xx:xx:... ip ...\")"""
    for l in lines:
        m = re.search(r'bssid ([0-9a-f:]{17})', l)
        if m: return m.group(1)
    raise RuntimeError('no bssid in %r' % (lines,))


class Peer:
    def __init__(self, port=None):
        self.port = port or os.environ.get('PEER_PORT', BOX3)
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = self.port, 115200, 0.1
        self.s.dtr = self.s.rts = False                 # never touch DTR/RTS: it resets the board
        self.s.open()

    def cmd(self, line, timeout=10.0):
        """send one command, return the output lines before its ok/err; raises on err or timeout"""
        self.s.reset_input_buffer()
        self.s.write((line + '\n').encode())
        out, buf, t0 = [], b'', time.time()
        while time.time() - t0 < timeout:
            buf += self.s.read(4096)
            while b'\n' in buf:
                l, buf = buf.split(b'\n', 1)
                l = l.decode(errors='replace').rstrip()
                if l == 'ok': return out
                if l.startswith('err'): raise RuntimeError(f'peer: {line!r} -> {l}')
                if l: out.append(l)
        raise TimeoutError(f'peer: no answer to {line!r} in {timeout} s')

    def close(self): self.s.close()
