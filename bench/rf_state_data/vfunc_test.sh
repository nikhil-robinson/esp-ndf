#!/bin/bash
# usage: vfunc_test.sh MASK -> cold D0 (UART): our init, then vendor rf_init (1) / bb_init (2), our TX
cd /home/nikhil/esp-ndf/bench/idf_rfrx
source ~/idf5_3_3/.espressif/v5.3.5/esp-idf/export.sh >/dev/null 2>&1
S=/tmp/claude-1000/-home-nikhil-esp-ndf/95a089d9-03e3-45bc-9b5a-b7019bc5f9f0/scratchpad/phy
U=/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0; R=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_24:58:7C:E2:C9:48-if00
rm -f sdkconfig; VFUNC=$1 TXLOOP=1 OURS=1 SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.uart" idf.py -B buildvf$1 build 2>&1 | grep -E "error:|undefined" ; rm -f sdkconfig
(cd buildvf$1; esptool.py -p $U --after no_reset write_flash @flash_args >/dev/null 2>&1)
python3 $S/cap.py $R 30 > $S/r.txt & sleep 0.3; python3 $S/cap.py $U 15 > $S/t.txt; wait
echo "vfunc $1: tone $(tr -d '\r' < $S/r.txt | grep -a '^t ' | awk '{print $5}' | sort -n | tail -1) | $(tr -d '\r' < $S/t.txt | grep -a 'vendor pieces\|Guru\|abort' | head -2 | tr '\n' ' ')"
rm -rf buildvf$1
