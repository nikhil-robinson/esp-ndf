#!/bin/sh
# App trace over JTAG check (examples/trace_test, already flashed): OpenOCD "esp apptrace" captures while the
# console reports the device side; then every record (u32 seq + pattern) is verified. Usage: trace_test.sh SERIAL OUT
SER=${1:-24:58:7C:E2:C9:D0}; OUT=${2:-/tmp/ndf_trace.bin}
PORT=$(ls /dev/serial/by-id/*$SER*)
OO=$(ls -d ~/.espressif/tools/openocd-esp32/*/openocd-esp32 | tail -1)
sleep 3                                                    # the board settles after a flash / reset
rm -f "$OUT"
timeout 50 $OO/bin/openocd -s $OO/share/openocd/scripts -c "adapter serial $SER" -c "set ESP_FLASH_SIZE 0" \
    -c "set ESP_RTOS none" -f board/esp32s3-builtin.cfg -c init -c "esp apptrace start file://$OUT 0 -1 8 0 0" \
    -c "sleep 20000" -c shutdown > "$OUT.log" 2>&1 &
python3 - "$PORT" <<'PY'
import serial, sys, time
c = serial.Serial(); c.port = sys.argv[1]; c.timeout = 0.5; c.dtr = False; c.rts = False
for _ in range(20):
    try: c.open(); break
    except Exception: time.sleep(0.5)
t = time.time()
while time.time() - t < 35:
    l = c.readline()
    if l: print('device:', l.decode(errors='replace').rstrip())
PY
wait
grep -E "Size is|Error" "$OUT.log" | grep -v Examination
python3 - "$OUT" <<'PY'
import sys, struct
d = open(sys.argv[1], 'rb').read() if __import__('os').path.exists(sys.argv[1]) else b''
bad = sum(d[s*1020:(s+1)*1020] != struct.pack('<I', s) + bytes((s*31+i) & 255 for i in range(4, 1020)) for s in range(len(d) // 1020))
print('=== trace %s: %d records, %d bad ===' % ('ALL PASS' if len(d) == 64*1020 and not bad else 'FAILED', len(d) // 1020, bad))
PY
