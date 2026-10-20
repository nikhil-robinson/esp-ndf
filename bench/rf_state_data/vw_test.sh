#!/bin/bash
# usage: vw_test.sh LO HI  -> cold TX test on D0 (UART) with vendor_words[LO..HI] applied after our init; prints tone dB
cd /home/nikhil/esp-ndf
source ~/idf5_3_3/.espressif/v5.3.5/esp-idf/export.sh >/dev/null 2>&1
S=/tmp/claude-1000/-home-nikhil-esp-ndf/95a089d9-03e3-45bc-9b5a-b7019bc5f9f0/scratchpad/phy
U=/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0; R=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_24:58:7C:E2:C9:48-if00
printf '#define NDF_CONSOLE NDF_CONSOLE_UART0\n#define VW_LO %d\n#define VW_HI %d\n#define VW_ANALOG 1\n' $1 $2 > examples/rf_ota_uart/ndf_config.h
rm -rf build/rf_ota_uart; make APP=examples/rf_ota_uart >/dev/null 2>&1 || { echo build-fail; exit 1; }
esptool.py --chip esp32s3 -p $U --after no_reset write_flash 0x0 build/rf_ota_uart/app.bin >/dev/null 2>&1
sleep 1; python3 $S/cap.py $R 40 > $S/r.txt & sleep 0.3; python3 $S/cap.py $U 40 > $S/t.txt; wait
echo "words $1..$2: tone $(tr -d '\r' < $S/r.txt | grep -a '^t ' | awk '{print $5}' | sort -n | tail -1)"
