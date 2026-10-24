#!/bin/sh
# Debug an esp-ndf app over the built-in USB-JTAG: tools/gdb.sh build/<app>/app.elf [adapter-serial] [gdb args...]
# Starts OpenOCD (flash probe and RTOS awareness off: we have neither), then gdb on :3333 (CALL0 frames unwind
# normally). Example: tools/gdb.sh build/runtime_test/app.elf 24:58:7C:E2:C9:D0 -ex "thbreak main" -ex c
ELF=$1; SER=$2; shift 2 2>/dev/null
OO=$(ls -d ~/.espressif/tools/openocd-esp32/*/openocd-esp32 | tail -1)
GDB=$(ls ~/.espressif/tools/xtensa-esp-elf-gdb/*/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb | tail -1)
$OO/bin/openocd -s $OO/share/openocd/scripts ${SER:+-c "adapter serial $SER"} -c "set ESP_FLASH_SIZE 0" \
    -c "set ESP_RTOS none" -f board/esp32s3-builtin.cfg > /tmp/ndf_openocd.log 2>&1 &
OPID=$!; sleep 3
$GDB -q -ex "target extended-remote :3333" -ex "monitor reset halt" "$@" "$ELF"
kill $OPID 2>/dev/null
