#!/usr/bin/env python3
"""Drive the command line of tests/runtime_test over the USB-JTAG console: help, add, quoted echo,
backspace editing, unknown command. Usage: repl_test.py PORT (app already running, prompt '> ')."""
import serial, sys, time
c = serial.Serial(); c.port = sys.argv[1]; c.baudrate = 115200; c.timeout = 0.3; c.dtr = False; c.rts = False; c.open()
def cmd(s):
    c.reset_input_buffer(); c.write(s.encode()); time.sleep(0.3)
    return c.read(4096).decode(errors='replace')
fails = 0
def check(name, ok, out):
    global fails
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else '  got: ' + repr(out)))
    fails += not ok
out = cmd('help\r');                 check('help lists add + echo', 'add' in out and 'echo' in out, out)
out = cmd('add 2 40\r');             check('add 2 40 -> 42', '\n42' in out.replace('\r', ''), out)
out = cmd('add 9 9X\b\b9\r');        check('backspace editing: "add 9 9X<bs><bs>9" -> 18', '\n18' in out.replace('\r', ''), out)
out = cmd('nosuch\r');               check('unknown command reported', 'unknown command: nosuch' in out, out)
out = cmd('add 1 2\x03');            check('Ctrl-C drops the line', '^C' in out and '\n3' not in out.replace('\r', ''), out)
print('=== repl %s (%d failures) ===' % ('ALL PASS' if not fails else 'FAILED', fails))
sys.exit(1 if fails else 0)
