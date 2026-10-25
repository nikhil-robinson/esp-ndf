#!/bin/bash
# ndf vs IDF USB CDC: 3 runs each, 4 MB both directions, same board/port/host script. Results -> bench/usb_cdc/data/cmp.txt
cd "$(dirname "$0")/.." || exit 1
port() { ls /dev/serial/by-id/usb-Espressif_USB_JTAG* 2>/dev/null | head -1; }
waitp() { timeout ${1:-60} bash -c 'until ls /dev/serial/by-id/usb-Espressif_USB_JTAG* >/dev/null 2>&1; do sleep 0.3; done'; }
waitp || { echo "no USB-JTAG port"; exit 1; }
(cd bench/idf_usbcdc/build && esptool.py --chip esp32s3 -p "$(port)" -b 460800 --after watchdog_reset write_flash @flash_args >/dev/null 2>&1) || { echo "IDF flash failed"; exit 1; }
for r in 1 2 3; do
  echo "IDF run $r: $(timeout 80 /usr/bin/python3 tools/usb_cdc_test.py 25 4 --idf 2>&1 | grep -v '^device: ' | tr '\n' ' ')"
  waitp 60 || { echo "USB-JTAG did not come back"; exit 1; }
  [ $r -lt 3 ] && esptool.py --chip esp32s3 -p "$(port)" --after watchdog_reset chip_id >/dev/null 2>&1
done
waitp 60; esptool.py --chip esp32s3 -p "$(port)" -b 460800 --after watchdog_reset write_flash 0x0 examples/usb/usb_cdc/build/app.bin >/dev/null 2>&1 || { echo "ndf flash failed"; exit 1; }
waitp; sleep 3
for r in 1 2 3; do
  echo "ndf run $r: $(timeout 80 /usr/bin/python3 tools/usb_cdc_test.py 25 4 2>&1 | grep -v '^device: ' | tr '\n' ' ')"
  waitp 60 || { echo "USB-JTAG did not come back"; exit 1; }; sleep 2
done
