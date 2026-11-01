"""Minimal OpenOCD telnet client for JTAG captures of the IDF peer (bench/wifi_regs/ocd_capture_ampdu.py).  usage: with Ocd(serial_suffix) as o: o.cmd('halt')"""
import os, socket, subprocess, time, re
OD = os.path.expanduser('~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20251215/openocd-esp32')
class Ocd:
    def __init__(self, usb_serial, port=4444, tcl=6666, gdb=3333):
        self.p = subprocess.Popen([OD + '/bin/openocd', '-s', OD + '/share/openocd/scripts', '-f', 'board/esp32s3-builtin.cfg', '-c', 'adapter serial %s' % usb_serial,
                                   '-c', 'telnet_port %d' % port, '-c', 'tcl_port %d' % tcl, '-c', 'gdb_port %d' % gdb], stdout=subprocess.DEVNULL, stderr=open('/tmp/ocd.log', 'w'))
        for _ in range(50):
            try: self.s = socket.create_connection(('127.0.0.1', tcl)); break           # the Tcl port: replies carry no asynchronous target messages
            except OSError: time.sleep(0.2)
        else: raise RuntimeError('openocd did not start: see /tmp/ocd.log')
        self.s.settimeout(60)
    def cmd(self, c):
        self.s.sendall(c.encode() + b'\x1a'); buf = b''
        while not buf.endswith(b'\x1a'): buf += self.s.recv(65536)
        return buf[:-1].decode(errors='replace').strip()
    def mdw(self, addr, n):                                   # [] when the address is not readable
        r = self.cmd('read_memory 0x%x 32 %d' % (addr, n)); return [int(x, 0) for x in r.split()] if r[:1].isdigit() else []
    def reg(self, name): return int(self.cmd('get_reg {%s}' % name).split()[1], 0)
    def __enter__(self): return self
    def __exit__(self, *a):
        try: self.s.sendall(b'shutdown\x1a')
        except OSError: pass
        self.p.wait(10)
