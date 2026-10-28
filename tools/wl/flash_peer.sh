#!/bin/bash
# Build the ESP-IDF peer (bench/idf_wl_peer) with ESP-IDF v5.3.5 and flash it to the BOX-3 only. usage: flash_peer.sh [build-only | devkit]  (default: the BOX-3; "devkit" flashes the same peer image to the DEVKIT for same-air IDF comparisons - esp-ndf must be reflashed afterwards)
cd "$(dirname "$0")/../../bench/idf_wl_peer"
BOX3=$(grep '^PEER' ../../make/boards.mk | sed 's/.*= *//'); [ -n "$BOX3" ] || BOX3=$(grep '^BOX3' ../../make/boards.mk | sed 's/.*= *//'); [ "$1" = devkit ] && BOX3=$(grep '^DEVKIT' ../../make/boards.mk | sed 's/.*= *//')
source /home/nikhil/idf5_3_3/.espressif/v5.3.5/esp-idf/export.sh >/dev/null 2>&1
idf.py build > /tmp/idf_peer_build.log 2>&1 || { tail -15 /tmp/idf_peer_build.log; echo "PEER BUILD FAILED"; exit 1; }; tail -2 /tmp/idf_peer_build.log
[ "$1" = build-only ] && exit 0
[ -e "$BOX3" ] || { echo "BOX-3 port missing"; exit 1; }
(cd build && esptool.py --chip esp32s3 -p "$BOX3" -b 460800 --after watchdog_reset write_flash @flash_args 2>&1 | tail -2)
