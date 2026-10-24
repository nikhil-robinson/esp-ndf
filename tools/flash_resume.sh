#!/bin/bash
# Recover a flash chip left in a suspended erase/program (status SR2 bit 7 set: esptool writes fail with an MD5
# mismatch): over the built-in USB-JTAG, halt the CPU, turn SPI1 auto-suspend off and send resume (0x7A) through SPI1.
OO=$(ls -d ~/.espressif/tools/openocd-esp32/*/openocd-esp32 | tail -1)
"$OO/bin/openocd" -s "$OO/share/openocd/scripts" -f board/esp32s3-builtin.cfg -c "init; halt; \
  mww 0x600020A0 0; mww 0x60002098 0x14; mww 0x6000209C 0; mww 0x60002008 0; mww 0x60002018 0x800000c0; \
  mww 0x6000201C 0; mww 0x60002020 0x7000007a; mww 0x60002024 0; mww 0x60002028 0; mww 0x60002000 0x40000; \
  sleep 500; resume; shutdown" >/dev/null 2>&1
sleep 1
esptool.py --chip esp32s3 -p "$(ls /dev/ttyACM* | head -1)" read-flash-status --bytes 2 2>&1 | grep -i "status:"
