"""Line-command client for the ESP-IDF peer (bench/idf_wl_peer) on the BOX-3.
The peer answers every command with output lines and then "ok" or "err <why>"."""
import os, sys, time, serial

BOX3 = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:30:F9:5A:C1:10-if00'


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
