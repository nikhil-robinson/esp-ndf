#!/bin/bash
# Boot time reset -> main: esp-ndf (examples/boot_time, XIP off/lib) vs IDF (bench/idf_boot), same RTC-timer code,
# 3 runs each, same board. Results -> bench/boot/result.txt
cd "$(dirname "$0")/.." || exit 1
port() { ls /dev/serial/by-id/usb-Espressif_USB_JTAG* 2>/dev/null | head -1; }
run() {   # $1 label; image already flashed
  for r in 1 2 3; do
    esptool.py --chip esp32s3 -p "$(port)" --after no_reset write_mem 0x50001000 0 >/dev/null 2>&1
    esptool.py --chip esp32s3 -p "$(port)" --after watchdog_reset chip_id >/dev/null 2>&1; sleep 2
    esptool.py --chip esp32s3 -p "$(port)" --after no_reset dump_mem 0x50001000 0x14 /tmp/rb_$$.bin >/dev/null 2>&1
    python3 -c "import struct; w=struct.unpack('<5I',open('/tmp/rb_$$.bin','rb').read()); print('$1 run $r: %s reset -> main %d us (cal %d)'%('ok' if w[0]==0x444F4E45 else 'BAD', w[4], w[3]))"
  done
}
(cd bench/idf_boot/build && esptool.py --chip esp32s3 -p "$(port)" -b 460800 --after no_reset write_flash @flash_args >/dev/null 2>&1); run "IDF"
for x in off lib; do make APP=examples/boot_time XIP=$x >/dev/null 2>&1
  esptool.py --chip esp32s3 -p "$(port)" -b 460800 --after no_reset write_flash 0x0 build/boot_time/app.bin $( [ $x != off ] && echo 0x300000 build/boot_time/xip.bin ) >/dev/null 2>&1; run "ndf XIP=$x"; done
