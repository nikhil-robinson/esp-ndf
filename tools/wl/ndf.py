"""Line-command client for an esp-ndf test app on the DEVKIT (opening the port resets the board: wait for the banner first)."""
import os, time, serial

DEVKIT = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_24:58:7C:E2:C9:D0-if00'


class Ndf:
    def __init__(self, port=None, ready='ready', timeout=15.0):
        self.port = port or os.environ.get('NDF_PORT', DEVKIT)
        self.s = serial.Serial(); self.s.port, self.s.baudrate, self.s.timeout = self.port, 115200, 0.1
        self.s.dtr = self.s.rts = False
        self.s.open(); self.buf = b''
        self.read_until(ready, timeout)

    def read_until(self, marker, timeout):
        out, t0 = [], time.time()
        while time.time() - t0 < timeout:
            self.buf += self.s.read(4096)
            while b'\n' in self.buf:
                l, self.buf = self.buf.split(b'\n', 1); l = l.decode(errors='replace').rstrip()
                out.append(l)
                if l == marker or (marker != 'ok' and marker in l): return out
        raise TimeoutError(f'ndf: no {marker!r} in {timeout} s; got {out[-3:]}')

    def cmd(self, line, timeout=10.0):
        """send a command, return the output lines before its 'ok'"""
        self.s.write((line + '\n').encode())
        return self.read_until('ok', timeout)[:-1]

    def close(self): self.s.close()
